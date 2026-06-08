package com.mrpowergamerbr.butterscotch;

import android.content.Context;
import android.opengl.GLSurfaceView;

/**
 * GLSurfaceView pinned to an OpenGL ES 1.x context.
 *
 * Gingerbread (API 10) ships GLSurfaceView; we explicitly request an
 * ES 1.1-capable EGL config (RGB565, no depth — the 2D renderer never
 * uses a depth buffer, and RGB565 halves the framebuffer bandwidth on the
 * Adreno 220, which matters at the Sensation XE's qHD 540x960 panel).
 *
 * setEGLContextClientVersion is deliberately NOT called: its default is
 * ES 1.x, which is exactly the fixed-function pipeline gles1_renderer.c
 * targets. (Calling it with 1 throws on some old drivers, so we leave the
 * default.)
 */
public class GameGLSurfaceView extends GLSurfaceView {

    public GameGLSurfaceView(Context context, String dataWinPath, String saveDir) {
        super(context);

        // RGB565, no alpha/depth/stencil — cheapest config the 2D path needs.
        setEGLConfigChooser(5, 6, 5, 0, 0, 0);

        GameRenderer renderer = new GameRenderer(dataWinPath, saveDir);
        setRenderer(renderer);
        setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
    }
}
