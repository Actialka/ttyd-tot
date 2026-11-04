# Hooks for the game's dialog / message window drawing functions, used to
# detect when a dialog box (e.g. the file select screen's copy / delete
# prompts) is on screen. Each hook only sets a flag, then jumps to the original
# function with all argument registers (including float registers) untouched.

.macro WINDOW_DISP_HOOK name
.global \name
\name:
lis %r11, g_FileRenameDialogDrawn@ha
li %r12, 1
stw %r12, g_FileRenameDialogDrawn@l (%r11)
lis %r11, \name\()_trampoline@ha
lwz %r12, \name\()_trampoline@l (%r11)
mtctr %r12
bctr
.endm

WINDOW_DISP_HOOK windowDispGX_Message_hook
WINDOW_DISP_HOOK windowDispGX_System_hook
WINDOW_DISP_HOOK windowDispGX_Kanban_hook
WINDOW_DISP_HOOK msgWindow_Disp_hook
