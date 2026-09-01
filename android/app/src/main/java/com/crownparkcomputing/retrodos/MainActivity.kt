package com.crownparkcomputing.retrodos

import android.os.Bundle
import org.libsdl.app.SDLActivity
import java.io.File

/**
 * DOSBox-X (SDL3) on Android.
 *
 * The core is a full DOSBox-X built against SDL3, so it draws and takes input
 * through SDL itself -- this activity only has to load it and hand it a
 * config. SDLActivity dlsym's SDL_main out of the library named by
 * getMainSharedObject(); src/gui/retrodos_host.cpp provides it.
 */
class MainActivity : SDLActivity() {

    /** Load order matters: the core has libSDL3 as a NEEDED entry. */
    override fun getLibraries(): Array<String> = arrayOf("SDL3", "retrodos")

    /** Not libmain.so -- our core keeps its own name. */
    override fun getMainSharedObject(): String =
        "${applicationInfo.nativeLibraryDir}/libretrodos.so"

    override fun getMainFunction(): String = "SDL_main"

    override fun getArguments(): Array<String> =
        arrayOf("-conf", confFile.absolutePath, "-defaultdir", dosRoot.absolutePath)

    private val dosRoot: File by lazy { File(getExternalFilesDir(null), "dos").apply { mkdirs() } }
    private val confFile: File by lazy { File(filesDir, "dosbox-x.conf") }

    override fun onCreate(savedInstanceState: Bundle?) {
        writeConf()
        super.onCreate(savedInstanceState)
    }

    private fun writeConf() {
        // 'working directory option=noprompt' is NOT optional. With a non-TTY
        // stdin DOSBox-X decides to prompt for a working directory and blocks
        // in a folder-picker dialog. On Android there is no stdin AND no
        // dialog, so it hangs with no diagnostic whatsoever.
        //
        // memsize=32 rather than more: DOS/4GW 1.97 miscalculates with large
        // memory and a pile of early-90s titles fail to start.
        confFile.writeText(
            """
            [sdl]
            autolock=false
            waitonerror=false
            [dosbox]
            working directory option=noprompt
            memsize=32
            title=Retro-DOS
            [cpu]
            core=dynamic
            cycles=max
            [sblaster]
            sbtype=sbpro2
            [autoexec]
            mount C "${dosRoot.absolutePath}"
            C:
            echo Retro-DOS ready. Put games in Android/data/${packageName}/files/dos
            """.trimIndent()
        )
    }
}
