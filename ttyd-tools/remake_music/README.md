# Remake battle music (optional)

The Music Toggle key item cycles **Off → Original → Remake**. In Remake mode,
tower battles play remake-OST tracks you convert yourself and add to your
extracted disc. No soundtrack audio is included in this repository.

Any track you don't add falls back to the original music, so you can add
tracks a few at a time.

## Which tracks play when

| Section | 32-floor run | 64-floor run |
|---|---|---|
| 1 | floors 1–8 | floors 1–16 |
| 2 | floors 9–16 | floors 17–32 |
| 3 | floors 17–24 | floors 33–48 |
| 4 | floors 25–32 | floors 49–64 |

Name your FLAC files like this (`N` = section 1–4):

| File | Plays |
|---|---|
| `sN_battle.flac` | normal battle, no first strike |
| `sN_mario_first.flac` | Mario / partner struck first (falls back to `sN_battle`) |
| `sN_enemy_first.flac` | enemy struck first (falls back to `sN_battle`) |
| `sN_victory.flac` | victory fanfare after a normal battle |
| `midboss.flac`, `midboss_victory.flac` | midbosses |
| `boss_doopliss.flac`, `boss_doopliss_victory.flac` | floor 32 boss (64-floor runs) and Doopliss |
| `boss_hooktail.flac` / `boss_gloomtail.flac` / `boss_bonetail.flac` (+ `_victory`) | the dragons |
| `boss_bowser.flac`, `boss_bowser_victory.flac` | Bowser |

Gold Fuzzy keeps its original music.

Album tracks normally play their loop twice and then fade out; the converter
finds the loop point automatically from that repeat. Use FLAC rather than
MP3 (MP3 adds a small gap at the start that throws off the loop).

## Setup (macOS)

1. Install Apple's command-line tools (for the C compiler):

   ```bash
   xcode-select --install
   ```

2. Install [Homebrew](https://brew.sh) if you don't have it, then:

   ```bash
   brew install ffmpeg python
   ```

   ```bash
   python3 -m pip install --user numpy
   ```

   If pip refuses with an "externally managed environment" error, use a
   virtual environment instead:

   ```bash
   python3 -m venv ~/tot-music-venv && ~/tot-music-venv/bin/pip install numpy
   ```

   and run the converter with `~/tot-music-venv/bin/python3` below.

3. Extract the patched game in Dolphin (right-click the game → Properties →
   Filesystem → right-click the disc → Extract Entire Disc…). Boot it from
   then on with File → Open → `sys/main.dol` inside the extracted folder.

## Converting

`convert.py` is self-contained, so you can download just that one file.

```bash
python3 convert.py /path/to/flac/folder /path/to/extracted/disc
```

This writes `files/sound/stream/tot_*_32k.stm` into the extracted disc.
Convert just some tracks with `--only s1_battle s1_victory`, and adjust
loudness for all tracks with e.g. `--gain -3` (dB) if they're too loud.

Then copy the new `gon.rel` to `files/rel/gon.rel`, boot the game, and set
the Music Toggle to Remake (its description shows the current mode).
