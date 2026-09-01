/*
 * Retro-Dosbox (sdl3 branch): SDL2-source-compatibility header over SDL3.
 *
 * WHY THIS EXISTS
 * ---------------
 * DOSBox-X's own plan for SDL3 is in include/dosbox.h:
 *
 *   // HACK: To make SDL3 porting easier, define SDL2 to prevent SDL1 code
 *   //       from compiling
 *   #if defined(C_SDL3) && !defined(C_SDL2)
 *   # define C_SDL2 1
 *   #endif
 *
 * i.e. keep the SDL2-era code paths compiling and put SDL3 underneath them,
 * rather than adding a third #if branch to ~3000 call sites (sdlmain.cpp alone
 * has 532). This header is the "underneath" half.
 *
 * THE RENAMES ARE NOT OUR PROBLEM
 * -------------------------------
 * SDL3 ships <SDL3/SDL_oldnames.h> with ~1032 SDL2 -> SDL3 name mappings,
 * gated on SDL_ENABLE_OLD_NAMES (which we set in CPPFLAGS). That covers the
 * entire mechanical-rename pile -- SDL_FreeSurface, SDL_CondWait,
 * SDL_AllocPalette, SDL_RenderCopy, SDL_UpperBlit and the rest -- officially
 * and maintained by SDL upstream. Do NOT hand-roll those here; a local #define
 * would only shadow a better one.
 *
 * Note what happens WITHOUT the flag: old names map to e.g.
 * SDL_FreeSurface_renamed_SDL_DestroySurface, so you get a compile error that
 * names the replacement. That is a feature -- it is how you find genuinely
 * changed APIs rather than silently mistranslating them.
 *
 * SO WHAT IS LEFT FOR THIS FILE
 * -----------------------------
 * Only what SDL_oldnames.h deliberately does NOT cover: APIs whose SEMANTICS
 * changed, not merely their spelling. Those cannot be a rename, because a
 * rename would compile and then behave differently. Every entry below earns
 * its place with a comment saying what actually changed.
 *
 * The dangerous class to watch for while porting: SDL3 flipped many functions
 * from "0 == success" to "true == success". An inverted check still compiles
 * AND still runs. SDL_oldnames.h does not and cannot save you from that.
 */
#ifndef RETRODOSBOX_SDL3COMPAT_SDL_H
#define RETRODOSBOX_SDL3COMPAT_SDL_H

#ifndef SDL_ENABLE_OLD_NAMES
/* Also set in CPPFLAGS by configure; belt and braces for any TU that reaches
 * this header first. Must precede <SDL3/SDL.h>. */
#define SDL_ENABLE_OLD_NAMES 1
#endif

#include <SDL3/SDL.h>

/* ---------------------------------------------------------------------- */
/* Spellings SDL_oldnames.h does not cover                                 */
/* ---------------------------------------------------------------------- */

/* SDL2 exported DECLSPEC; SDL3 namespaced it to SDL_DECLSPEC. This is not in
 * SDL_oldnames.h because it is a build macro rather than an API name, yet it
 * accounts for ~85% of the errors in this tree: the vendored SDL_ttf.h and
 * SDL_sound.h headers use it in every prototype (SNDDECLSPEC is defined in
 * terms of DECLSPEC, so this fixes both). */
#ifndef DECLSPEC
#define DECLSPEC SDL_DECLSPEC
#endif

/* ---------------------------------------------------------------------- */
/* Semantic changes SDL_oldnames.h does not cover                          */
/* ---------------------------------------------------------------------- */

/* SDL2: SDL_SetRelativeMouseMode(SDL_bool) was global.
 * SDL3: relative mode is per-window. There is no global form to rename to. */
#ifndef SDL_SetRelativeMouseMode
#define SDL_SetRelativeMouseMode(b) SDL_SetWindowRelativeMouseMode(NULL,(b))
#endif

/* SDL2: SDL_FreeFormat() released a per-surface SDL_PixelFormat object.
 * SDL3: SDL_Surface::format is a plain enum; there is nothing to free. */
#ifndef SDL_FreeFormat
#define SDL_FreeFormat(x) ((void)(x))
#endif

#endif /* RETRODOSBOX_SDL3COMPAT_SDL_H */
