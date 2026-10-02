#include "SDL.h"

// Icon is provided by the .desktop file (see res/v06c-debugger.desktop.in).
// SDL_SetWindowIcon is not used — GNOME ignores _NET_WM_ICON in favour of
// .desktop file icons.  This stub satisfies the linker for tv.cpp which
// calls icon_set() from the main emulator codebase.
void icon_set(SDL_Window *) {}
