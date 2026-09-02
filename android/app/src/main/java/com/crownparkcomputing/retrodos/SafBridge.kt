package com.crownparkcomputing.retrodos

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import androidx.documentfile.provider.DocumentFile
import java.io.File

/**
 * Storage Access Framework bridge.
 *
 * Android will not hand an app broad file access -- MANAGE_EXTERNAL_STORAGE is
 * reserved for file managers -- so a library kept outside the app's own
 * directories is reachable only as a *document tree* the user explicitly
 * grants.
 *
 * That creates a problem the emulator cannot solve on its own: SAF yields
 * `content://` URIs, and DOSBox-X mounts a DIRECTORY BY PATH. There is no
 * `mount C content://...`. So this bridge does two separate jobs:
 *
 *   1. ENUMERATE over SAF, which is enough to build the launcher list. No
 *      real path is needed just to show names.
 *   2. STAGE the one game being launched into the app's own directory, which
 *      IS a real path. A DOS game is typically a few megabytes, so copying the
 *      selected title costs little -- and nothing else is ever copied.
 *
 * Everything here is called from native code via JNI, so the methods are
 * static and take/return only primitives and strings.
 */
object SafBridge {

    private const val TAG   = "retrodos"
    private const val PREFS = "retrodos_saf"
    private const val KEY   = "tree_uri"

    /** Set by MainActivity so the bridge can reach a Context and the picker. */
    @Volatile @JvmStatic var activity: MainActivity? = null

    private fun ctx(): Context? = activity?.applicationContext

    /* ---------------------------------------------------------------- */
    /* Grant                                                             */
    /* ---------------------------------------------------------------- */

    /** Launch the system folder picker. Returns immediately; the grant lands
     *  asynchronously, so native code polls treeUri(). */
    @JvmStatic
    fun pick() {
        val a = activity ?: return
        a.runOnUiThread { a.launchFolderPicker() }
    }

    /** The persisted tree URI, or "" if the user has not granted one (or the
     *  grant has been revoked, which happens if the SD card is reformatted). */
    @JvmStatic
    fun treeUri(): String {
        val c = ctx() ?: return ""
        val saved = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(KEY, null) ?: return ""

        /* A saved string is not a live grant. Check it is still in the
         * persisted list, otherwise the launcher would show a library the app
         * can no longer read. */
        val uri = Uri.parse(saved)
        val held = c.contentResolver.persistedUriPermissions.any {
            it.uri == uri && it.isReadPermission
        }
        return if (held) saved else ""
    }

    /** Called by MainActivity when the user picks a folder. */
    @JvmStatic
    fun onFolderPicked(uri: Uri) {
        val c = ctx() ?: return
        try {
            /* Persist, or the grant dies with the process and the user is
             * asked again on every launch. */
            c.contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (e: SecurityException) {
            Log.w(TAG, "could not persist grant: $e")
        }
        c.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putString(KEY, uri.toString()).apply()
        Log.i(TAG, "SAF folder granted: $uri")
    }

    /* ---------------------------------------------------------------- */
    /* Enumerate                                                         */
    /* ---------------------------------------------------------------- */

    /** Names of the sub-folders of the granted tree: one per game.
     *  Returns an empty array when nothing is granted. */
    @JvmStatic
    fun listGames(): Array<String> {
        val c = ctx() ?: return emptyArray()
        val uriStr = treeUri()
        if (uriStr.isEmpty()) return emptyArray()

        val root = DocumentFile.fromTreeUri(c, Uri.parse(uriStr)) ?: return emptyArray()

        /* DocumentFile.listFiles() is notoriously slow -- one IPC per child --
         * so query the children in a single cursor instead. On a 3000-entry
         * library that is the difference between a moment and a minute. */
        return try {
            val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(
                root.uri, DocumentsContract.getTreeDocumentId(root.uri))
            val out = ArrayList<String>(4096)
            c.contentResolver.query(
                childrenUri,
                arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                        DocumentsContract.Document.COLUMN_MIME_TYPE),
                null, null, null
            )?.use { cur ->
                while (cur.moveToNext()) {
                    val name = cur.getString(0) ?: continue
                    val mime = cur.getString(1) ?: ""
                    /* A collection mixes loose folders and archives, so both
                     * count as a game. The archive is extracted at launch;
                     * the name shown drops the extension. */
                    if (mime == DocumentsContract.Document.MIME_TYPE_DIR) out.add(name)
                    else if (name.endsWith(".zip", true)) out.add(name)
                }
            }
            Log.i(TAG, "listGames: cursor gave ${out.size} entries")

            /* Fall back to DocumentFile.listFiles() when the bulk cursor comes
             * back empty. It is far slower -- an IPC per child -- but it uses a
             * different provider path, and some providers refuse the
             * children-of-tree query for a granted SUBFOLDER while still
             * answering the per-document walk. Better slow than empty. */
            if (out.isEmpty()) {
                for (f in root.listFiles()) {
                    val n = f.name ?: continue
                    if (f.isDirectory || n.endsWith(".zip", true)) out.add(n)
                }
                Log.i(TAG, "listGames: listFiles fallback gave ${out.size} entries")
            }

            out.sortWith(String.CASE_INSENSITIVE_ORDER)
            Log.i(TAG, "listGames: ${out.size} entries under $uriStr")
            out.toTypedArray()
        } catch (e: Exception) {
            Log.w(TAG, "listGames failed: $e")
            emptyArray()
        }
    }

    /* ---------------------------------------------------------------- */
    /* Stage                                                             */
    /* ---------------------------------------------------------------- */

    /**
     * Copy one game's folder out of the granted tree into [destDir], a real
     * filesystem path the emulator can mount. Returns true on success.
     *
     * Skipped entirely when the destination already holds files, so launching
     * the same game twice costs nothing after the first time.
     */
    @JvmStatic
    fun stage(gameName: String, destDir: String): Boolean {
        val c = ctx() ?: return false
        val uriStr = treeUri()
        if (uriStr.isEmpty()) return false

        val dest = File(destDir)
        if (dest.isDirectory && (dest.list()?.isNotEmpty() == true)) return true
        if (!dest.isDirectory && !dest.mkdirs()) return false

        val root = DocumentFile.fromTreeUri(c, Uri.parse(uriStr)) ?: return false
        val src = root.findFile(gameName) ?: run {
            Log.w(TAG, "stage: '$gameName' not found in the granted tree")
            return false
        }
        return try {
            if (src.isDirectory) copyTree(c, src, dest) else unzip(c, src, dest)
            Log.i(TAG, "staged '$gameName' -> $destDir")
            true
        } catch (e: Exception) {
            Log.w(TAG, "stage failed for '$gameName': $e")
            false
        }
    }

    /** Extract an archive straight out of SAF, without a temporary copy. */
    private fun unzip(c: Context, src: DocumentFile, dest: File) {
        c.contentResolver.openInputStream(src.uri)?.use { raw ->
            java.util.zip.ZipInputStream(raw.buffered()).use { zin ->
                while (true) {
                    val e = zin.nextEntry ?: break
                    val out = File(dest, e.name)
                    /* Reject paths that escape the destination: a crafted
                     * archive with ../ entries would otherwise write anywhere
                     * the app can reach. */
                    if (!out.canonicalPath.startsWith(dest.canonicalPath + File.separator) &&
                        out.canonicalPath != dest.canonicalPath) {
                        Log.w(TAG, "unzip: skipping suspicious entry ${e.name}")
                        zin.closeEntry(); continue
                    }
                    if (e.isDirectory) out.mkdirs()
                    else {
                        out.parentFile?.mkdirs()
                        out.outputStream().use { zin.copyTo(it, 64 * 1024) }
                    }
                    zin.closeEntry()
                }
            }
        }
    }

    private fun copyTree(c: Context, src: DocumentFile, dest: File) {
        for (child in src.listFiles()) {
            val name = child.name ?: continue
            val out = File(dest, name)
            if (child.isDirectory) {
                out.mkdirs()
                copyTree(c, child, out)
            } else {
                c.contentResolver.openInputStream(child.uri)?.use { input ->
                    out.outputStream().use { output -> input.copyTo(output, 64 * 1024) }
                }
            }
        }
    }
}
