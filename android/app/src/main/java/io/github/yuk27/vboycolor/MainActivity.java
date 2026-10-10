package io.github.yuk27.vboycolor;

import android.app.NativeActivity;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.util.Log;
import android.view.HapticFeedbackConstants;
import android.view.View;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Arrays;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;

// Extends NativeActivity (native side still loads as before) only to get the
// onCreate/onActivityResult lifecycle callbacks the SAF folder picker
// (ACTION_OPEN_DOCUMENT_TREE) needs - see platform/android/AndroidPlatform.h
// for why SAF. The methods below are called from native code via JNI
// (platform/android/AndroidPlatform.h); they're plain polled methods, so no
// JNI_OnLoad/RegisterNatives is needed.
public class MainActivity extends NativeActivity {
    private static final String TAG = "VBoyColor";
    private static final String PREFS_NAME = "vboycolor";
    private static final String PREF_ROMS_TREE_URI = "roms_tree_uri";
    private static final int REQUEST_PICK_ROMS_FOLDER = 1001;
    // Settings > Folders (core/io/DataFolders.h): a folder per kind of file -
    // 0 games (the ROMs folder above), 1 saved games, 2 save states,
    // 3 settings. Unset = the default place inside the games folder (the
    // root for saved games, its "States" subfolder for the other two).
    private static final int KIND_GAMES = 0, KIND_SAVES = 1, KIND_STATES = 2, KIND_SETTINGS = 3;
    private static final String PREF_FOLDER_URI = "folder_uri_"; // + kind (1-3)
    // A folder to pick when the app next starts (a headset can't show the
    // picker mid-session - see requestPickRomsFolder).
    private static final String PREF_PICK_PENDING = "pick_pending";
    private static final int REQUEST_PICK_FOLDER = 1100; // + kind
    private final Object folderLock = new Object();
    private volatile String folderStatus = null;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        int pending = prefs.getInt(PREF_PICK_PENDING, -1);
        if (getRomsTreeUriString() == null) {
            requestPickRomsFolder();
        } else if (pending >= 0) {
            prefs.edit().remove(PREF_PICK_PENDING).apply();
            launchFolderPicker(pending);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode >= REQUEST_PICK_FOLDER && requestCode < REQUEST_PICK_FOLDER + 4) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                folderPicked(requestCode - REQUEST_PICK_FOLDER, data.getData());
            }
            return;
        }
        if (requestCode != REQUEST_PICK_ROMS_FOLDER || resultCode != RESULT_OK || data == null) {
            return;
        }
        Uri treeUri = data.getData();
        if (treeUri == null) {
            return;
        }

        // Persistable so it survives restarts (the point of SAF: pick once).
        // Read+write, since save states/SRAM go back into this folder.
        getContentResolver().takePersistableUriPermission(treeUri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);

        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        prefs.edit().putString(PREF_ROMS_TREE_URI, treeUri.toString()).apply();
        Log.d(TAG, "ROMs folder set: " + treeUri);
    }

    // Launches the folder picker. Only called from onCreate, before any VR
    // session exists: launching it while already immersed makes Horizon OS
    // drop VR focus to the system shell and never hand it back.
    private void requestPickRomsFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQUEST_PICK_ROMS_FOLDER);
    }

    // A phone or tablet: the game full screen, the system's bars out of the
    // way (a swipe from the edge brings them back for a moment).
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !isHeadset()) {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }
    }

    // ---- Called from native code (platform/android/AndroidPlatform.h) via JNI ----

    // A light tap of the phone's vibration motor (an on-screen button pressed).
    public void haptic() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                getWindow().getDecorView().performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY,
                        HapticFeedbackConstants.FLAG_IGNORE_VIEW_SETTING);
            }
        });
    }

    // 0-100 device battery level, for the in-menu battery indicator.
    public int getBatteryLevel() {
        Intent batteryIntent = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (batteryIntent == null) {
            return -1;
        }
        int level = batteryIntent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
        int scale = batteryIntent.getIntExtra(BatteryManager.EXTRA_SCALE, -1);
        if (level < 0 || scale <= 0) {
            return -1;
        }
        return (int) (level / (float) scale * 100);
    }

    // Lets the user pick a different folder after first launch. Only clears
    // the saved folder - relaunching mid-session loses VR focus (see
    // requestPickRomsFolder), so the user restarts manually and onCreate's
    // picker fires again (getRomsTreeUriString is now null).
    public void requestChangeRomsFolder() {
        String saved = getRomsTreeUriString();
        if (saved != null) {
            try {
                getContentResolver().releasePersistableUriPermission(Uri.parse(saved),
                        Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            } catch (Exception e) {
                Log.e(TAG, "releasePersistableUriPermission failed", e);
            }
        }
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().remove(PREF_ROMS_TREE_URI).apply();
        Log.d(TAG, "ROMs folder cleared - restart the app to pick a new one");
    }

    // null if no folder is picked or it's no longer accessible - checked
    // against the OS's persisted-permission list, not just the cached pref.
    public String getRomsTreeUriString() {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        String saved = prefs.getString(PREF_ROMS_TREE_URI, null);
        if (saved == null) {
            return null;
        }
        Uri treeUri = Uri.parse(saved);
        for (UriPermission perm : getContentResolver().getPersistedUriPermissions()) {
            if (perm.getUri().equals(treeUri) && perm.isReadPermission()) {
                return saved;
            }
        }
        return null;
    }

    // Flat [name0, uri0, name1, uri1, ...] for every ".vb" file directly in
    // the chosen folder (non-recursive) - flat to keep the JNI side simple.
    public String[] listRomFiles() {
        String treeUriString = getRomsTreeUriString();
        if (treeUriString == null) {
            return new String[0];
        }
        Uri treeUri = Uri.parse(treeUriString);
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, DocumentsContract.getTreeDocumentId(treeUri));

        List<String> results = new ArrayList<>();
        ContentResolver resolver = getContentResolver();
        try (Cursor cursor = resolver.query(childrenUri, new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME
        }, null, null, null)) {
            if (cursor != null) {
                while (cursor.moveToNext()) {
                    String documentId = cursor.getString(0);
                    String displayName = cursor.getString(1);
                    if (displayName == null || !displayName.toLowerCase().endsWith(".vb")) {
                        continue;
                    }
                    String name = displayName.substring(0, displayName.length() - 3);
                    Uri docUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId);
                    results.add(name);
                    results.add(docUri.toString());
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "listRomFiles failed", e);
        }
        return results.toArray(new String[0]);
    }

    // Returns a detached raw fd (caller/native owns it and must close() it)
    // for read-only access to the given document URI string, or -1 on
    // failure.
    public int openRomFileDescriptor(String documentUriString) {
        try {
            Uri uri = Uri.parse(documentUriString);
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
            if (pfd == null) {
                return -1;
            }
            return pfd.detachFd();
        } catch (Exception e) {
            Log.e(TAG, "openRomFileDescriptor failed: " + documentUriString, e);
            return -1;
        }
    }

    // ---- Save-state/SRAM I/O inside the ROMs folder (.srm in the root,
    // .state/.stateimg in a "States" subfolder). Unlike ROMs, these are
    // looked up (or created) by name each time, since SAF hands back no
    // stable URI for a file that doesn't exist yet. ----

    // documentId of parentDocumentId's child named displayName, or null.
    private String findChildDocumentId(Uri treeUri, String parentDocumentId, String displayName) {
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentDocumentId);
        try (Cursor cursor = getContentResolver().query(childrenUri, new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME
        }, null, null, null)) {
            if (cursor != null) {
                while (cursor.moveToNext()) {
                    if (displayName.equals(cursor.getString(1))) {
                        return cursor.getString(0);
                    }
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "findChildDocumentId failed: " + displayName, e);
        }
        return null;
    }

    // documentId of the ROMs folder's "States" subfolder, creating it if it
    // doesn't exist yet. null on failure.
    private String getOrCreateStatesDirDocumentId(Uri treeUri) {
        String rootDocId = DocumentsContract.getTreeDocumentId(treeUri);
        String existing = findChildDocumentId(treeUri, rootDocId, "States");
        if (existing != null) {
            return existing;
        }
        try {
            Uri parentUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, rootDocId);
            Uri createdUri = DocumentsContract.createDocument(getContentResolver(), parentUri,
                    DocumentsContract.Document.MIME_TYPE_DIR, "States");
            return createdUri != null ? DocumentsContract.getDocumentId(createdUri) : null;
        } catch (Exception e) {
            Log.e(TAG, "getOrCreateStatesDirDocumentId failed", e);
            return null;
        }
    }

    // Where a kind of file lives: its own folder if one is chosen, else the
    // games folder (its root for games and saved games, "States" for the
    // rest). {treeUri, parentDocumentId}, or null (createIfMissing: make
    // the "States" subfolder if needed).
    private Object[] locate(int kind, boolean createIfMissing) {
        String custom = kind == KIND_GAMES ? null : getFolderUriString(kind);
        if (custom != null) {
            Uri tree = Uri.parse(custom);
            return new Object[]{tree, DocumentsContract.getTreeDocumentId(tree)};
        }
        String games = getRomsTreeUriString();
        if (games == null) {
            return null;
        }
        Uri tree = Uri.parse(games);
        if (kind == KIND_GAMES || kind == KIND_SAVES) {
            return new Object[]{tree, DocumentsContract.getTreeDocumentId(tree)};
        }
        String states = createIfMissing ? getOrCreateStatesDirDocumentId(tree)
                : findChildDocumentId(tree, DocumentsContract.getTreeDocumentId(tree), "States");
        return states == null ? null : new Object[]{tree, states};
    }

    // Opens (creating fileName - and the games folder's "States" subfolder,
    // where it goes there - if they don't exist yet) a writable fd for a
    // kind's file, truncating any existing content. Returns a detached raw
    // fd (caller/native owns it and must close() it), or -1 on failure or
    // if no folder is picked.
    public int openDataFileForWrite(int kind, String fileName) {
        try {
            Object[] where = locate(kind, true);
            if (where == null) {
                return -1;
            }
            Uri treeUri = (Uri) where[0];
            String parentDocId = (String) where[1];
            String fileDocId = findChildDocumentId(treeUri, parentDocId, fileName);
            Uri fileUri;
            if (fileDocId != null) {
                fileUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, fileDocId);
            } else {
                Uri parentUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, parentDocId);
                fileUri = DocumentsContract.createDocument(getContentResolver(), parentUri, "application/octet-stream", fileName);
                if (fileUri == null) {
                    return -1;
                }
            }
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(fileUri, "wt");
            return pfd == null ? -1 : pfd.detachFd();
        } catch (Exception e) {
            Log.e(TAG, "openDataFileForWrite failed: " + fileName, e);
            return -1;
        }
    }

    // Read counterpart to openDataFileForWrite - -1 if fileName doesn't
    // exist or no folder is picked.
    public int openDataFileForRead(int kind, String fileName) {
        try {
            Object[] where = locate(kind, false);
            if (where == null) {
                return -1;
            }
            Uri treeUri = (Uri) where[0];
            String fileDocId = findChildDocumentId(treeUri, (String) where[1], fileName);
            if (fileDocId == null) {
                return -1;
            }
            ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(
                    DocumentsContract.buildDocumentUriUsingTree(treeUri, fileDocId), "r");
            return pfd == null ? -1 : pfd.detachFd();
        } catch (Exception e) {
            Log.e(TAG, "openDataFileForRead failed: " + fileName, e);
            return -1;
        }
    }

    public boolean dataFileExists(int kind, String fileName) {
        Object[] where = locate(kind, false);
        return where != null && findChildDocumentId((Uri) where[0], (String) where[1], fileName) != null;
    }

    // ---- Settings > Folders ----

    // null if none is chosen or it's no longer accessible.
    private String getFolderUriString(int kind) {
        String saved = getSharedPreferences(PREFS_NAME, MODE_PRIVATE).getString(PREF_FOLDER_URI + kind, null);
        if (saved == null) {
            return null;
        }
        Uri treeUri = Uri.parse(saved);
        for (UriPermission perm : getContentResolver().getPersistedUriPermissions()) {
            if (perm.getUri().equals(treeUri) && perm.isWritePermission()) {
                return saved;
            }
        }
        return null;
    }

    // A tree's own name ("VB games"), or its URI's last part.
    private String folderName(String treeUriString) {
        Uri tree = Uri.parse(treeUriString);
        try (Cursor cursor = getContentResolver().query(
                DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree)),
                new String[]{DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && cursor.getString(0) != null) {
                return cursor.getString(0);
            }
        } catch (Exception e) {
            Log.w(TAG, "folderName failed", e);
        }
        String id = DocumentsContract.getTreeDocumentId(tree);
        int at = Math.max(id.lastIndexOf('/'), id.lastIndexOf(':'));
        return at >= 0 && at + 1 < id.length() ? id.substring(at + 1) : id;
    }

    // What the menu shows for a kind's folder: its name, "" for the default
    // place.
    public String getFolderLabel(int kind) {
        String uri = kind == KIND_GAMES ? getRomsTreeUriString() : getFolderUriString(kind);
        return uri == null ? "" : folderName(uri);
    }

    // The last change's outcome, once ("" if none).
    public String takeFolderStatus() {
        String status = folderStatus;
        folderStatus = null;
        return status == null ? "" : status;
    }

    // A headset (the VR app) or a phone/tablet (the flat app with touch
    // controls - see platform/android/PhoneMain.cpp).
    public boolean isHeadset() {
        return getPackageManager().hasSystemFeature("android.hardware.vr.headtracking")
                || "Oculus".equalsIgnoreCase(Build.MANUFACTURER) || "Meta".equalsIgnoreCase(Build.MANUFACTURER);
    }

    // Asks for a kind's folder: the system's picker now, or on a headset the
    // next time the app starts (see requestPickRomsFolder). Returns a line
    // for the menu.
    public String pickFolder(final int kind) {
        if (isHeadset()) {
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putInt(PREF_PICK_PENDING, kind).apply();
            return "Restart VBoy Color to choose it";
        }
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                launchFolderPicker(kind);
            }
        });
        return "";
    }

    private void launchFolderPicker(int kind) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQUEST_PICK_FOLDER + kind);
    }

    // The picker's answer: copy the kind's files over (on a thread of its
    // own), then switch to the new folder.
    private void folderPicked(final int kind, final Uri treeUri) {
        try {
            getContentResolver().takePersistableUriPermission(treeUri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        } catch (Exception e) {
            Log.e(TAG, "takePersistableUriPermission failed", e);
            folderStatus = "Can't use that folder - nothing changed";
            return;
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                changeFolder(kind, treeUri.toString());
            }
        }).start();
    }

    // Back to the default place (kinds 1-3), copying the kind's files there.
    public String resetFolder(final int kind) {
        if (kind == KIND_GAMES || getFolderUriString(kind) == null) {
            return "";
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                changeFolder(kind, null);
            }
        }).start();
        return "Copying...";
    }

    // Points a kind at newTree (null: back to the default place) after
    // copying its files over - and, for the games folder, those of the
    // kinds that live in it by default. The originals stay.
    private void changeFolder(int kind, String newTree) {
        synchronized (folderLock) {
            SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
            Object[][] before = new Object[4][];
            for (int k = 0; k < 4; k++) {
                before[k] = locate(k, false);
            }
            String oldTree = kind == KIND_GAMES ? getRomsTreeUriString() : getFolderUriString(kind);
            SharedPreferences.Editor edit = prefs.edit();
            String key = kind == KIND_GAMES ? PREF_ROMS_TREE_URI : PREF_FOLDER_URI + kind;
            if (newTree == null) {
                edit.remove(key);
            } else {
                edit.putString(key, newTree);
            }
            edit.commit();
            int copied = 0;
            for (int k = 1; k < 4; k++) {
                if (k != kind && kind != KIND_GAMES) {
                    continue;
                }
                if (kind == KIND_GAMES && getFolderUriString(k) != null) {
                    continue; // (has a folder of its own)
                }
                Object[] after = locate(k, true);
                if (before[k] == null || after == null) {
                    continue;
                }
                copied += copyKind((Uri) before[k][0], (String) before[k][1], (Uri) after[0], (String) after[1], k);
            }
            // The old folder's permission, if nothing else uses it.
            if (oldTree != null && !oldTree.equals(newTree)) {
                boolean used = oldTree.equals(getRomsTreeUriString());
                for (int k = 1; k < 4; k++) {
                    used |= oldTree.equals(getFolderUriString(k));
                }
                if (!used) {
                    try {
                        getContentResolver().releasePersistableUriPermission(Uri.parse(oldTree),
                                Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                    } catch (Exception e) {
                        Log.w(TAG, "releasePersistableUriPermission failed", e);
                    }
                }
            }
            String[] names = {"Games", "Saved games", "Save states", "Settings"};
            folderStatus = copied > 0 ? names[kind] + ": copied " + copied + (copied == 1 ? " file" : " files") + " - the originals stay"
                    : names[kind] + " folder changed";
        }
    }

    // core/io/DataFolders.cpp's DataKindOf: which kind a file is, by its
    // name and whether it's in (the default) "States".
    private static int kindOf(String name, boolean inStates) {
        int dot = name.lastIndexOf('.');
        String ext = dot >= 0 ? name.substring(dot + 1).toLowerCase() : "";
        if (!inStates) {
            return ext.equals("srm") ? KIND_SAVES : KIND_GAMES;
        }
        return ext.startsWith("state") ? KIND_STATES : KIND_SETTINGS;
    }

    private byte[] readAll(Uri uri) {
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null) {
                return null;
            }
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[65536];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
            return out.toByteArray();
        } catch (Exception e) {
            return null;
        }
    }

    // Copies a kind's files from one folder to another: those it doesn't
    // have, and those newer than its own (unless the same) - the one replaced
    // is kept as "<name>.bak". Returns how many.
    private int copyKind(Uri fromTree, String fromDoc, Uri toTree, String toDoc, int kind) {
        if (fromTree.equals(toTree) && fromDoc.equals(toDoc)) {
            return 0;
        }
        boolean inStates = kind == KIND_STATES || kind == KIND_SETTINGS;
        ContentResolver resolver = getContentResolver();
        int copied = 0;
        try (Cursor cursor = resolver.query(DocumentsContract.buildChildDocumentsUriUsingTree(fromTree, fromDoc), new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_LAST_MODIFIED
        }, null, null, null)) {
            while (cursor != null && cursor.moveToNext()) {
                String name = cursor.getString(1);
                if (name == null || DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2))
                        || kindOf(name, inStates) != kind) {
                    continue;
                }
                Uri source = DocumentsContract.buildDocumentUriUsingTree(fromTree, cursor.getString(0));
                byte[] bytes = readAll(source);
                if (bytes == null) {
                    continue;
                }
                String existing = findChildDocumentId(toTree, toDoc, name);
                if (existing != null) {
                    Uri target = DocumentsContract.buildDocumentUriUsingTree(toTree, existing);
                    long sourceTime = cursor.getLong(3), targetTime = 0;
                    try (Cursor t = resolver.query(target, new String[]{DocumentsContract.Document.COLUMN_LAST_MODIFIED}, null, null, null)) {
                        if (t != null && t.moveToFirst()) {
                            targetTime = t.getLong(0);
                        }
                    }
                    if (sourceTime <= targetTime || Arrays.equals(bytes, readAll(target))) {
                        continue;
                    }
                    String backup = findChildDocumentId(toTree, toDoc, name + ".bak");
                    if (backup != null) {
                        DocumentsContract.deleteDocument(resolver, DocumentsContract.buildDocumentUriUsingTree(toTree, backup));
                    }
                    if (DocumentsContract.renameDocument(resolver, target, name + ".bak") == null) {
                        continue;
                    }
                }
                Uri created = DocumentsContract.createDocument(resolver, DocumentsContract.buildDocumentUriUsingTree(toTree, toDoc),
                        "application/octet-stream", name);
                if (created == null) {
                    continue;
                }
                try (OutputStream out = resolver.openOutputStream(created, "wt")) {
                    if (out != null) {
                        out.write(bytes);
                        copied++;
                    }
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "copyKind failed", e);
        }
        return copied;
    }

    // ---- Downloads (the library's optional box art): one at a time, on a
    // thread of its own; native code polls (AndroidPlatform::PollDownload). ----

    private static final int MAX_DOWNLOAD_BYTES = 16 * 1024 * 1024;
    private final Object downloadLock = new Object();
    private int downloadState = -1; // 0 running, 1 done, -1 failed/none
    private byte[] downloadBytes;

    public boolean startDownload(final String url) {
        synchronized (downloadLock) {
            if (downloadState == 0) {
                return false;
            }
            downloadState = 0;
            downloadBytes = null;
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                runDownload(url);
            }
        }).start();
        return true;
    }

    private void runDownload(String url) {
        byte[] result = null;
        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) new URL(url).openConnection();
            connection.setConnectTimeout(10000);
            connection.setReadTimeout(20000);
            connection.setInstanceFollowRedirects(true);
            if (connection.getResponseCode() == 200) {
                try (InputStream in = connection.getInputStream()) {
                    ByteArrayOutputStream out = new ByteArrayOutputStream();
                    byte[] buffer = new byte[16384];
                    int read;
                    while ((read = in.read(buffer)) > 0 && out.size() <= MAX_DOWNLOAD_BYTES) {
                        out.write(buffer, 0, read);
                    }
                    if (out.size() > 0 && out.size() <= MAX_DOWNLOAD_BYTES) {
                        result = out.toByteArray();
                    }
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "download failed: " + url, e);
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
        synchronized (downloadLock) {
            downloadBytes = result;
            downloadState = result != null ? 1 : -1;
        }
    }

    // 0 running, 1 done (then takeDownload), -1 failed or none.
    public int pollDownload() {
        synchronized (downloadLock) {
            return downloadState;
        }
    }

    public byte[] takeDownload() {
        synchronized (downloadLock) {
            byte[] bytes = downloadBytes;
            downloadBytes = null;
            downloadState = -1;
            return bytes;
        }
    }
}
