package com.mrpowergamerbr.butterscotch;

/**
 * Thin Java mirror of the JNI entry points in {@code jni/bs_jni.c}.
 *
 * Every method here is implemented natively in libbutterscotch.so. The
 * contract matches the iOS BSGameViewController lifecycle:
 *
 *   load()            -> parse data.win, build VM + renderer + runner + audio
 *   surfaceChanged()  -> remember the GL framebuffer size
 *   step()            -> advance + draw one display tick (call from GL thread)
 *   keyDown/keyUp()   -> feed the GML keyboard (UI thread; guarded natively)
 *   touch()           -> feed a normalized touch into the GML mouse
 *   onTrimMemory()    -> evict GL atlases under memory pressure
 *   teardown()        -> free the whole runtime
 *
 * GML virtual-key codes (subset used by the on-screen pad) live in
 * {@link #VK_LEFT} etc. and match src/runner_keyboard.h exactly.
 */
public final class NativeBridge {

    static {
        System.loadLibrary("butterscotch");
    }

    private NativeBridge() {}

    // ---- GML virtual key codes (mirror src/runner_keyboard.h) ----
    public static final int VK_ENTER  = 13;
    public static final int VK_SHIFT  = 16;
    public static final int VK_ESCAPE = 27;
    public static final int VK_SPACE  = 32;
    public static final int VK_LEFT   = 37;
    public static final int VK_UP     = 38;
    public static final int VK_RIGHT  = 39;
    public static final int VK_DOWN   = 40;
    // GML uppercases letter keys: ord('Z') == 90, etc.
    public static final int VK_Z = 'Z';
    public static final int VK_X = 'X';
    public static final int VK_C = 'C';

    /**
     * Parse data.win and stand up the runtime. MUST be called on the GL
     * thread (a valid GLES 1.1 context must be current — the renderer
     * allocates its FBO during the first frame, but create() inspects GL
     * caps immediately).
     *
     * @param dataWinPath absolute path to the extracted data.win
     * @param saveDir     absolute path to a writable per-game save dir
     * @return empty string on success, otherwise a human-readable error
     */
    public static native String nativeLoad(String dataWinPath, String saveDir);

    /** Record the GL framebuffer size (call from onSurfaceChanged). */
    public static native void nativeSurfaceChanged(int width, int height);

    /** Advance + draw one display tick. Call from the GL thread each frame. */
    public static native void nativeStep();

    /** Press a GML key. Safe to call from the UI thread. */
    public static native void nativeKeyDown(int gmlKey);

    /** Release a GML key. Safe to call from the UI thread. */
    public static native void nativeKeyUp(int gmlKey);

    /** Feed a normalized [0,1] touch position into the GML mouse. */
    public static native void nativeTouch(float normX, float normY);

    /** Memory pressure hook: evict non-current GL atlases. */
    public static native void nativeOnTrimMemory();

    /** Free the whole runtime. Call from the GL thread. */
    public static native void nativeTeardown();

    /** Build version (git short hash), surfaced in the About screen. */
    public static native String nativeVersion();

    // ---- Convenience wrappers (nicer call sites) ----
    public static String load(String dataWinPath, String saveDir) { return nativeLoad(dataWinPath, saveDir); }
    public static void surfaceChanged(int w, int h) { nativeSurfaceChanged(w, h); }
    public static void step() { nativeStep(); }
    public static void keyDown(int k) { nativeKeyDown(k); }
    public static void keyUp(int k) { nativeKeyUp(k); }
    public static void touch(float x, float y) { nativeTouch(x, y); }
    public static void onTrimMemory() { nativeOnTrimMemory(); }
    public static void teardown() { nativeTeardown(); }
    public static String version() { return nativeVersion(); }
}
