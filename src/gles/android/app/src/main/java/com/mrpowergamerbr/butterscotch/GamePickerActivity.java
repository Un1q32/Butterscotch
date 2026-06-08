package com.mrpowergamerbr.butterscotch;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Environment;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Landing screen: lists every game we can find (a folder containing a
 * {@code data.win}) and launches {@link GameActivity} on tap.
 *
 * This is the Android analogue of the iOS BSFolderBrowser / game picker.
 * It is intentionally styled like Android (Holo-era dark list rows with a
 * top bar) rather than copying iOS chrome 1:1 — the brief was "looks like
 * the iOS one but in Android style".
 *
 * Search roots (first existing wins, all are scanned and merged):
 *   1. <external>/Butterscotch/        (user-droppable, e.g. /sdcard/Butterscotch)
 *   2. <external>/Android/data/<pkg>/files/games/   (app-scoped external)
 *   3. <filesDir>/games/               (internal, adb-pushable)
 *
 * Each game lives in its own subfolder, e.g.
 *   /sdcard/Butterscotch/undertale/data.win
 */
public class GamePickerActivity extends Activity {

    private static final String TAG = "Butterscotch";

    // Android-styled dark theme palette (Holo dark, period-correct for 2.3+
    // via AppCompat-free manual styling — keeps the APK tiny and dependency-free).
    private static final int COL_BG      = 0xFF121212;
    private static final int COL_BAR     = 0xFF1F1B16; // warm butterscotch-tinted bar
    private static final int COL_ACCENT  = 0xFFE8A93B; // butterscotch amber
    private static final int COL_ROW     = 0xFF1E1E1E;
    private static final int COL_ROW_ALT = 0xFF242424;
    private static final int COL_TEXT    = 0xFFEDEDED;
    private static final int COL_SUBTEXT = 0xFF9A9A9A;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        rebuild();
    }

    @Override
    protected void onResume() {
        super.onResume();
        rebuild(); // re-scan in case the user dropped a game in while away
    }

    private void rebuild() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(COL_BG);

        // ---- Top bar -------------------------------------------------------
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.VERTICAL);
        bar.setBackgroundColor(COL_BAR);
        bar.setPadding(dp(16), dp(18), dp(16), dp(14));

        TextView title = new TextView(this);
        title.setText("\uD83E\uDD67 Butterscotch");   // 🥧
        title.setTextColor(COL_ACCENT);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);
        title.setTypeface(title.getTypeface(), android.graphics.Typeface.BOLD);
        bar.addView(title);

        TextView subtitle = new TextView(this);
        subtitle.setText("GameMaker: Studio runner \u00B7 build " + safeVersion());
        subtitle.setTextColor(COL_SUBTEXT);
        subtitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        bar.addView(subtitle);

        root.addView(bar, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        // ---- Game list -----------------------------------------------------
        ScrollView scroll = new ScrollView(this);
        LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(list);

        List<File> games = findGames();
        if (games.isEmpty()) {
            list.addView(emptyState());
        } else {
            int i = 0;
            for (File dataWin : games) {
                list.addView(gameRow(dataWin, i % 2 == 0 ? COL_ROW : COL_ROW_ALT));
                i++;
            }
        }
        root.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        // ---- Footer hint ---------------------------------------------------
        TextView hint = new TextView(this);
        hint.setText("Drop a game folder (containing data.win) into:\n"
                + Environment.getExternalStorageDirectory() + "/Butterscotch/");
        hint.setTextColor(COL_SUBTEXT);
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        hint.setPadding(dp(16), dp(10), dp(16), dp(14));
        root.addView(hint);

        setContentView(root);
    }

    private View emptyState() {
        TextView tv = new TextView(this);
        tv.setText("No games found.\n\nCopy a GameMaker data.win (with its audiogroup*.dat\n"
                + "files, if any) into a folder under:\n\n"
                + Environment.getExternalStorageDirectory() + "/Butterscotch/<game>/data.win\n\n"
                + "then come back to this screen.");
        tv.setTextColor(COL_SUBTEXT);
        tv.setGravity(Gravity.CENTER);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        tv.setPadding(dp(24), dp(48), dp(24), dp(48));
        return tv;
    }

    private View gameRow(final File dataWin, int bgColor) {
        final File folder = dataWin.getParentFile();
        String name = folder != null ? folder.getName() : dataWin.getName();
        long sizeMb = dataWin.length() / (1024 * 1024);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setBackgroundColor(bgColor);
        row.setPadding(dp(16), dp(16), dp(16), dp(16));
        row.setClickable(true);

        TextView nameTv = new TextView(this);
        nameTv.setText(prettify(name));
        nameTv.setTextColor(COL_TEXT);
        nameTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        row.addView(nameTv);

        TextView pathTv = new TextView(this);
        pathTv.setText(dataWin.getParent() + "  \u00B7  " + sizeMb + " MB");
        pathTv.setTextColor(COL_SUBTEXT);
        pathTv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        row.addView(pathTv);

        // 1px divider
        View divider = new View(this);
        divider.setBackgroundColor(0xFF2E2E2E);

        LinearLayout wrap = new LinearLayout(this);
        wrap.setOrientation(LinearLayout.VERTICAL);
        wrap.addView(row, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        wrap.addView(divider, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 1));

        row.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                Intent it = new Intent(GamePickerActivity.this, GameActivity.class);
                it.putExtra(GameActivity.EXTRA_DATA_WIN, dataWin.getAbsolutePath());
                it.putExtra(GameActivity.EXTRA_TITLE, dataWin.getParentFile().getName());
                startActivity(it);
            }
        });
        return wrap;
    }

    // ---- Game discovery ----------------------------------------------------

    private List<File> findGames() {
        List<File> roots = new ArrayList<File>();
        File ext = Environment.getExternalStorageDirectory();
        if (ext != null) roots.add(new File(ext, "Butterscotch"));
        File extFiles = getExternalFilesDir(null);
        if (extFiles != null) roots.add(new File(extFiles, "games"));
        roots.add(new File(getFilesDir(), "games"));

        List<File> found = new ArrayList<File>();
        for (File root : roots) {
            scanForDataWin(root, found, 0);
        }
        // De-dup by absolute path and sort by folder name.
        Collections.sort(found, new java.util.Comparator<File>() {
            public int compare(File a, File b) {
                return a.getParentFile().getName().compareToIgnoreCase(b.getParentFile().getName());
            }
        });
        return found;
    }

    /** Recursively look for data.win up to a couple of levels deep. */
    private void scanForDataWin(File dir, List<File> out, int depth) {
        if (dir == null || !dir.isDirectory() || depth > 2) return;
        File direct = new File(dir, "data.win");
        if (direct.isFile()) { out.add(direct); return; } // a game folder; don't descend further
        File[] kids = dir.listFiles();
        if (kids == null) return;
        for (File k : kids) {
            if (k.isDirectory()) scanForDataWin(k, out, depth + 1);
        }
    }

    // ---- helpers -----------------------------------------------------------

    private String prettify(String folderName) {
        String s = folderName.replace('_', ' ').replace('-', ' ').trim();
        if (s.length() == 0) return folderName;
        // Title-case the first letter of each word for a friendlier label.
        StringBuilder sb = new StringBuilder();
        boolean cap = true;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            sb.append(cap ? Character.toUpperCase(c) : c);
            cap = (c == ' ');
        }
        return sb.toString();
    }

    private String safeVersion() {
        try { return NativeBridge.version(); }
        catch (Throwable t) { return "?"; }
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }
}
