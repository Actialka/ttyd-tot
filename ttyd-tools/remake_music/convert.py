#!/usr/bin/env python3
"""Converts remake-OST FLACs into looping TTYD .stm streams.

For each expected track (see TRACKS), this script:
  1. decodes the FLAC with ffmpeg,
  2. finds the loop by locating where the song repeats itself
     (album tracks play the loop twice, then fade out),
  3. resamples to 32 kHz (the loop body is resampled as a periodic signal,
     so the loop seam stays seamless),
  4. encodes it to GameCube DSP-ADPCM, and
  5. writes it as a .stm file in the same layout as the game's own streams.

Requirements: python3, numpy, ffmpeg, and a C compiler (cc).
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np

SR_IN = 48000
SR_OUT = 32000
FRAME = 14  # samples per DSP-ADPCM frame
# Vanilla streams' channel sizes are multiples of 0x20 bytes (4 frames).
ALIGN = FRAME * 4

# Output file stem -> whether the track is expected to loop.
TRACKS = {}
for _s in range(1, 5):
    for _v in ("battle", "mario_first", "enemy_first"):
        TRACKS["s%d_%s" % (_s, _v)] = True
    TRACKS["s%d_victory" % _s] = False
for _b in ("midboss", "boss_doopliss", "boss_hooktail", "boss_gloomtail",
           "boss_bonetail", "boss_bowser"):
    TRACKS[_b] = True
    TRACKS[_b + "_victory"] = False

HERE = os.path.dirname(os.path.abspath(__file__))
# Opaque trailing bytes present in every vanilla .stm channel header.
CH_HDR_TAIL = bytes.fromhex("3a0060a9400064ff120000000000a844f97707000000")


def die(msg):
    sys.exit("error: " + msg)


def build_encoder(workdir):
    exe = os.path.join(workdir, "dspenc")
    src = os.path.join(HERE, "dspenc.c")
    r = subprocess.run(["cc", "-O2", "-o", exe, src, "-lm"])
    if r.returncode:
        die("could not compile dspenc.c (is a C compiler installed?)")
    return exe


def decode_flac(path):
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-map", "0:a:0", "-ac", "2",
         "-ar", str(SR_IN), "-f", "s16le", "-"],
        stdout=subprocess.PIPE, check=True).stdout
    return np.frombuffer(raw, dtype=np.int16).reshape(-1, 2).astype(np.float64)


def resample_ffmpeg(x):
    """Plain (non-periodic) resample of a stereo float array to SR_OUT."""
    data = np.clip(np.round(x), -32768, 32767).astype("<i2").tobytes()
    out = subprocess.run(
        ["ffmpeg", "-v", "error", "-f", "s16le", "-ar", str(SR_IN), "-ac", "2",
         "-i", "-", "-ar", str(SR_OUT), "-f", "s16le", "-"],
        input=data, stdout=subprocess.PIPE, check=True).stdout
    return np.frombuffer(out, dtype=np.int16).reshape(-1, 2).astype(np.float64)


def resample_periodic(x, n_out):
    """Resamples one period of a periodic signal to exactly n_out samples."""
    n_in = len(x)
    spec = np.fft.rfft(x, axis=0)
    bins = n_out // 2 + 1
    out_spec = np.zeros((bins, x.shape[1]), dtype=complex)
    k = min(bins, spec.shape[0])
    out_spec[:k] = spec[:k]
    return np.fft.irfft(out_spec, n=n_out, axis=0) * (n_out / n_in)


def find_loop(x, min_loop_sec=10.0):
    """Returns (loop_start, loop_end) in input samples, or None if no loop."""
    mono = x.mean(axis=1)
    n = len(mono)
    hop, win = 1024, 2048
    frames = (n - win) // hop
    if frames < 64:
        return None
    w = np.hanning(win)
    idx = np.arange(frames)[:, None] * hop + np.arange(win)[None, :]
    feats = []
    for chunk in np.array_split(np.arange(frames), max(1, frames // 2000)):
        sp = np.abs(np.fft.rfft(mono[idx[chunk]] * w, axis=1))[:, :1024]
        feats.append(np.log1p(sp.reshape(len(chunk), 128, 8).sum(axis=2)))
    S = np.concatenate(feats)
    S -= S.mean(axis=1, keepdims=True)
    S /= np.linalg.norm(S, axis=1, keepdims=True) + 1e-9

    # Coarse search: the lag where a long stretch of the song repeats.
    W = int(8 * SR_IN / hop)
    best = (0.0, 0, 0)
    for lag in range(int(min_loop_sec * SR_IN / hop), frames - W):
        d = (S[:frames - lag] * S[lag:]).sum(axis=1)
        c = np.convolve(d, np.ones(W) / W, "valid")
        j = int(c.argmax())
        if c[j] > best[0]:
            best = (c[j], lag, j)
    score, lag_f, start_f = best
    if score < 0.95:
        return None

    # Refine the lag to the exact sample with a cross-correlation.
    lag0 = lag_f * hop
    center = (start_f + W // 2) * hop
    C, R = SR_IN, 2 * hop
    b = mono[center + lag0: center + lag0 + C]
    a = mono[center - R: center + C + R]
    if len(b) < C or len(a) < C + 2 * R:
        return None
    corr = np.array([np.dot(a[o:o + C], b) for o in range(2 * R + 1)])
    lag = lag0 - (int(corr.argmax()) - R)

    # Loop start: earliest sample from which the audio repeats `lag` later.
    diff = np.abs(x[:n - lag] - x[lag:]).max(axis=1)
    level = np.abs(x).max(axis=1)[:n - lag] + 1.0
    ok = diff <= np.maximum(64.0, level * 0.01)
    # Walk back from a known-good point to find where repetition begins.
    good = center
    if not ok[good: good + 4800].all():
        return None
    start = good
    block = 480
    while start - block >= 0 and ok[start - block: start].mean() > 0.98:
        start -= block
    while start > 0 and ok[start - 1]:
        start -= 1
    return start, start + lag


def encode(enc, workdir, chan, loop_start):
    raw = os.path.join(workdir, "ch.raw")
    out = os.path.join(workdir, "ch.bin")
    np.clip(np.round(chan), -32768, 32767).astype("<i2").tofile(raw)
    subprocess.run([enc, raw, out, str(loop_start)], check=True)
    b = open(out, "rb").read()
    return b[:32], b[32:38], b[38:]


def write_stm(path, nsamples, loop_start, channels):
    nframes = nsamples // FRAME
    ch_size = nframes * 8
    interleave = (ch_size + 0x20) // 0x20 * 0x20
    loop_off = (loop_start // FRAME) * 8 if loop_start >= 0 else None

    hdr = bytearray(0x100)
    struct.pack_into(">HHIIIII", hdr, 0, 0x0200, SR_OUT, 2, ch_size,
                     0xFFFFFFFF if loop_off is None else loop_off,
                     ch_size, ch_size)
    if loop_off is not None:
        struct.pack_into(">II", hdr, 0x18, loop_off, loop_off)
    nibbles = nframes * 16
    ls_nib = (loop_start // FRAME) * 16 + 2 if loop_start >= 0 else 2
    for c, (coefs, loopctx, data) in enumerate(channels):
        o = 0x40 + c * 0x60
        struct.pack_into(">IIIHHIII", hdr, o, nsamples, nibbles, SR_OUT, 1, 0,
                         ls_nib, nibbles + 2, 2)
        hdr[o + 0x1C:o + 0x3C] = coefs
        struct.pack_into(">HHhh", hdr, o + 0x3C, 0, data[0], 0, 0)
        hdr[o + 0x44:o + 0x4A] = loopctx
        hdr[o + 0x4A:o + 0x60] = CH_HDR_TAIL
    with open(path, "wb") as f:
        f.write(hdr)
        for _, _, data in channels:
            f.write(data.ljust(interleave, b"\0"))
        f.write(b"\0" * 0x7FE0)


def convert(enc, workdir, src, dst, should_loop, gain_db):
    x = decode_flac(src) * (10 ** (gain_db / 20))
    loop = find_loop(x) if should_loop else None
    if should_loop and loop is None:
        print("  warning: no repeat found; writing a non-looping track")
    if loop:
        s, e = loop
        # Loop length must be a whole number of ADPCM frames at 32 kHz; the
        # resulting pitch change is far below one cent.
        l_out = max(FRAME, int(round((e - s) * SR_OUT / SR_IN / FRAME)) * FRAME)
        body = resample_periodic(x[s:e], l_out)
        s_out = int(np.ceil(s * SR_OUT / SR_IN))
        intro = resample_ffmpeg(x[:s + SR_IN // 10])[:s_out]
        # Pad the start so the loop begins on a frame boundary and the
        # total length is a multiple of ALIGN (body is a multiple of FRAME).
        pad = (-(len(intro) + l_out)) % ALIGN
        y = np.concatenate([np.zeros((pad, 2)), intro, body])
        loop_start = pad + len(intro)
        print("  loop %.3fs -> %.3fs (%.2fs loop)" %
              (s / SR_IN, e / SR_IN, (e - s) / SR_IN))
    else:
        y = resample_ffmpeg(x)
        # Trim trailing near-silence, then pad to a whole frame.
        loud = np.nonzero(np.abs(y).max(axis=1) > 16)[0]
        if len(loud):
            y = y[:loud[-1] + 1]
        y = np.concatenate([y, np.zeros(((-len(y)) % ALIGN, 2))])
        loop_start = -1
    peak = np.abs(y).max()
    if peak > 32767:
        print("  warning: clipping (peak %.0f); try a negative --gain" % peak)
    chans = [encode(enc, workdir, y[:, c], loop_start) for c in range(2)]
    write_stm(dst, len(y), loop_start, chans)
    print("  wrote %s (%.1f MB)" % (dst, os.path.getsize(dst) / 1e6))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("flac_dir", help="folder containing s1_battle.flac etc.")
    ap.add_argument("disc_dir",
                    help="extracted disc folder (the one containing 'files' "
                         "and 'sys'); output goes to files/sound/stream")
    ap.add_argument("--only", nargs="*", help="convert only these tracks")
    ap.add_argument("--gain", type=float, default=0.0,
                    help="volume change in dB applied to every track")
    args = ap.parse_args()

    out_dir = os.path.join(args.disc_dir, "files", "sound", "stream")
    if not os.path.isdir(out_dir):
        die("%s not found; point disc_dir at the extracted disc" % out_dir)
    names = args.only or list(TRACKS)
    missing = [n for n in names
               if not os.path.exists(os.path.join(args.flac_dir, n + ".flac"))]
    if missing:
        print("missing (skipped): " + ", ".join(m + ".flac" for m in missing))

    with tempfile.TemporaryDirectory() as workdir:
        enc = build_encoder(workdir)
        for name in names:
            if name in missing:
                continue
            if name not in TRACKS:
                die("unknown track name: " + name)
            print(name)
            convert(enc, workdir, os.path.join(args.flac_dir, name + ".flac"),
                    os.path.join(out_dir, "tot_%s_32k.stm" % name),
                    TRACKS[name], args.gain)


if __name__ == "__main__":
    main()
