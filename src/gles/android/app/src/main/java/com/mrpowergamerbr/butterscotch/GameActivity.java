package com.mrpowergamerbr.butterscotch;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;

/**
 * Hosts a single running game: a GLSurfaceView (the game) with the
 * TouchOverlayView stacked on top.
 *
 * Android analogue of iOS's BSGameViewController. The data.win path is
 * passed in via the launching Intent ("dataWinPath"); the per-game save
 * directory is derived under the app-private files dir so writes survive
 * but stay sandboxed.
 *
 * Fullscreen + landscape are forced (the games are 640x480-ish and the
 * iOS port also locks landscape). The hardware Back button leaves the
 * game and returns to the picker, mirroring the iOS back chevron.
 */
public class GameActivity extends Activity {

    private static final String TAG = "Butterscotch";
    public static final String EXTRA_DATA_WIN = "dataWinPath";
    public static final String EXTRA_TITLE    = "title";

    private GameGLSurfaceView glView;
    private TouchOverlayView overlay;
    private String dataWinPath;
    private String saveDir;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Fullscreen, keep screen on, hide the status bar (matches iOS
        // UIStatusBarHidden=true). Landscape is pinned via the manifest.
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN,
                             WindowManager.LayoutParams.FLAG_FULLSCREEN);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        dataWinPath = getIntent().getStringExtra(EXTRA_DATA_WIN);
        if (dataWinPath == null) {
            Toast.makeText(this, "No game specified", Toast.LENGTH_LONG).show();
            finish();
            return;
        }

        // Per-game save dir: <filesDir>/saves/<gameFolderName>/
        java.io.File gameDir = new java.io.File(dataWinPath).getParentFile();
        String gameName = gameDir != null ? gameDir.getName() : "game";
        java.io.File saves = new java.io.File(getFilesDir(), "saves/" + gameName);
        saves.mkdirs();
        saveDir = saves.getAbsolutePath();
        Log.i(TAG, "GameActivity: data.win=" + dataWinPath + " saveDir=" + saveDir);

        FrameLayout root = new FrameLayout(this);

        glView = new GameGLSurfaceView(this, dataWinPath, saveDir);
        root.addView(glView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        overlay = new TouchOverlayView(this);
        root.addView(overlay, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        setContentView(root);
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (overlay != null) overlay.resetInput();
        if (glView != null) glView.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (glView != null) glView.onResume();
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        if (glView != null) {
            // Hop onto the GL thread; native atlas eviction touches GL state.
            glView.queueEvent(new Runnable() {
                public void run() { NativeBridge.onTrimMemory(); }
            });
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (glView != null) {
            final GameGLSurfaceView gv = glView;
            gv.queueEvent(new Runnable() {
                public void run() { NativeBridge.teardown(); }
            });
        }
    }
}
