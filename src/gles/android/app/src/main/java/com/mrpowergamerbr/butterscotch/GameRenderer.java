package com.mrpowergamerbr.butterscotch;

import android.opengl.GLSurfaceView;
import android.util.Log;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * Drives the Butterscotch runtime from the GLSurfaceView GL thread.
 *
 * This is the Android counterpart to the CADisplayLink-driven
 * {@code glViewTick:} loop in src/gles/ios/main.m. GLSurfaceView already
 * owns the EGL context and calls us back on a dedicated GL thread, so all
 * we do is forward lifecycle events into the native bridge:
 *
 *   onSurfaceCreated  -> (re)load the game; GL context just became current
 *   onSurfaceChanged  -> tell native the framebuffer size
 *   onDrawFrame       -> one tick (RENDERMODE_CONTINUOUSLY)
 *
 * The data.win/save paths are captured up front; the actual native load
 * happens on the GL thread inside onSurfaceCreated so a valid GLES 1.1
 * context is current when the renderer inspects GL caps and builds its FBO.
 */
public class GameRenderer implements GLSurfaceView.Renderer {

    private static final String TAG = "Butterscotch";

    private final String dataWinPath;
    private final String saveDir;

    /** Set on the GL thread; read by the host activity for error reporting. */
    public volatile String loadError = null;
    public volatile boolean loaded = false;

    public GameRenderer(String dataWinPath, String saveDir) {
        this.dataWinPath = dataWinPath;
        this.saveDir = saveDir;
    }

    @Override
    public void onSurfaceCreated(GL10 gl, EGLConfig config) {
        // The GL context was (re)created — e.g. first start, or after the
        // app returned from background and Android tore down the surface.
        // On a context loss the old native GL objects are already invalid,
        // so a fresh load is the safe path (matches iOS reload-on-appear).
        Log.i(TAG, "onSurfaceCreated: GL_RENDERER=" + gl.glGetString(GL10.GL_RENDERER)
                + " GL_VERSION=" + gl.glGetString(GL10.GL_VERSION));

        if (loaded) {
            // Context was lost and recreated: drop the stale runtime first.
            NativeBridge.teardown();
            loaded = false;
        }

        String err = NativeBridge.load(dataWinPath, saveDir);
        if (err != null && !err.isEmpty()) {
            loadError = err;
            Log.e(TAG, "native load failed: " + err);
        } else {
            loaded = true;
            loadError = null;
            Log.i(TAG, "native load ok (build " + NativeBridge.version() + ")");
        }
    }

    @Override
    public void onSurfaceChanged(GL10 gl, int width, int height) {
        Log.i(TAG, "onSurfaceChanged: " + width + "x" + height);
        NativeBridge.surfaceChanged(width, height);
    }

    @Override
    public void onDrawFrame(GL10 gl) {
        if (!loaded) return;
        NativeBridge.step();
    }
}
