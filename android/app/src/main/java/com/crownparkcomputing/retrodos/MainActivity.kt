package com.crownparkcomputing.retrodos

import org.libsdl.app.SDLActivity

/**
 * Retro-DOS.
 *
 * Deliberately thin. Everything -- the launcher, the config the engine is
 * handed, scaling, the in-game overlay -- lives in the native SDL3 + ImGui
 * frontend (frontend/retrodos_frontend.cpp), so it is shared with the desktop
 * build rather than reimplemented per platform.
 *
 * This class only tells SDLActivity which libraries to load and where SDL_main
 * lives. Load order matters: the core has libSDL3 as a NEEDED entry.
 */
class MainActivity : SDLActivity() {

    override fun getLibraries(): Array<String> = arrayOf("SDL3", "retrodos")

    /** Not libmain.so -- the core keeps its own name. */
    override fun getMainSharedObject(): String =
        "${applicationInfo.nativeLibraryDir}/libretrodos.so"

    override fun getMainFunction(): String = "SDL_main"

    /**
     * No arguments: the frontend discovers the library and generates the
     * engine's config itself. Passing a game in from here would put half the
     * launch logic on the Android side, where the desktop build cannot use it.
     */
    override fun getArguments(): Array<String> = arrayOf()
}
