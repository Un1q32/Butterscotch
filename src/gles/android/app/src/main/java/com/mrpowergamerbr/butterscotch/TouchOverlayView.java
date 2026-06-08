package com.mrpowergamerbr.butterscotch;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.AttributeSet;
import android.util.TypedValue;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;

/**
 * On-screen touch controls, drawn over the GLSurfaceView.
 *
 * This is the Android-styled equivalent of iOS's BSTouchOverlay
 * (src/gles/ios/main.m). Same control scheme — a left analog-ish D-pad
 * (8-direction, angle-based, with a centre dead zone) and three face
 * buttons Z / X / C on the right, plus Shift (hold to run) and a small
 * Esc/menu button — but rendered in a flat, translucent "Material"-ish
 * style instead of iOS's glassy rounded rects, and it tracks multiple
 * fingers (multi-touch pointers) so you can hold a direction and press a
 * face button at the same time.
 *
 * Key codes are pushed straight into the GML keyboard via NativeBridge.
 */
public class TouchOverlayView extends View {

    // ---- Visual tuning ----
    private static final int COL_PAD_FILL   = Color.argb(70, 255, 255, 255);
    private static final int COL_PAD_FILL_ON= Color.argb(150, 255, 255, 255);
    private static final int COL_PAD_RING   = Color.argb(120, 255, 255, 255);
    private static final int COL_BTN_FILL   = Color.argb(90, 0, 0, 0);
    private static final int COL_BTN_FILL_ON= Color.argb(160, 90, 200, 160);
    private static final int COL_BTN_RING   = Color.argb(150, 255, 255, 255);
    private static final int COL_LABEL      = Color.argb(235, 255, 255, 255);

    private final Paint fill   = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ring   = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint label  = new Paint(Paint.ANTI_ALIAS_FLAG);

    // ---- Geometry (computed in onSizeChanged) ----
    private float dpadCx, dpadCy, dpadRadius, dpadDead;
    private final RectF zBtn = new RectF();
    private final RectF xBtn = new RectF();
    private final RectF cBtn = new RectF();
    private final RectF shiftBtn = new RectF();
    private final RectF escBtn = new RectF();

    // ---- Live state ----
    // Currently-held direction keys from the D-pad (so we can release on move).
    private final ArrayList<Integer> dpadKeys = new ArrayList<Integer>(2);
    private boolean zOn, xOn, cOn, shiftOn;
    // pointerId -> which control it currently owns (-1 == dpad, else a VK_*).
    private final java.util.HashMap<Integer, Integer> pointerOwner = new java.util.HashMap<Integer, Integer>();

    private static final int OWNER_DPAD = -1;

    public TouchOverlayView(Context c) { super(c); init(); }
    public TouchOverlayView(Context c, AttributeSet a) { super(c, a); init(); }

    private void init() {
        ring.setStyle(Paint.Style.STROKE);
        ring.setStrokeWidth(dp(2));
        label.setColor(COL_LABEL);
        label.setTextAlign(Paint.Align.CENTER);
        label.setFakeBoldText(true);
        setFocusable(false);
        // Transparent so the game shows through.
        setBackgroundColor(Color.TRANSPARENT);
    }

    private float dp(float v) {
        return TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v,
                getResources().getDisplayMetrics());
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);

        float margin = dp(18);
        float btnSize = dp(58);
        float btnGap  = dp(14);

        // D-pad: bottom-left.
        dpadRadius = dp(78);
        dpadCx = margin + dpadRadius;
        dpadCy = h - margin - dpadRadius;
        dpadDead = dpadRadius * 0.28f;

        // Face buttons: bottom-right, Z / X / C left-to-right matching iOS.
        float ay = h - margin - btnSize;
        float axRight = w - margin - btnSize;
        cBtn.set(axRight, ay, axRight + btnSize, ay + btnSize);
        xBtn.set(axRight - (btnSize + btnGap), ay, axRight - btnGap, ay + btnSize);
        zBtn.set(axRight - 2 * (btnSize + btnGap), ay,
                 axRight - 2 * btnGap - btnSize, ay + btnSize);

        // Shift: above X (hold to run), smaller.
        float shSize = dp(48);
        shiftBtn.set(xBtn.centerX() - shSize / 2, ay - btnGap - shSize,
                     xBtn.centerX() + shSize / 2, ay - btnGap);

        // Esc/menu: top-left, small.
        float escW = dp(54), escH = dp(34);
        escBtn.set(margin, margin, margin + escW, margin + escH);

        label.setTextSize(dp(20));
    }

    // ---- Drawing ----
    @Override
    protected void onDraw(Canvas canvas) {
        // D-pad base ring.
        ring.setColor(COL_PAD_RING);
        canvas.drawCircle(dpadCx, dpadCy, dpadRadius, ring);

        // Directional wedges: highlight the held ones.
        drawDpadArrow(canvas, NativeBridge.VK_UP,    dpadCx, dpadCy - dpadRadius * 0.6f, 0);
        drawDpadArrow(canvas, NativeBridge.VK_DOWN,  dpadCx, dpadCy + dpadRadius * 0.6f, 180);
        drawDpadArrow(canvas, NativeBridge.VK_LEFT,  dpadCx - dpadRadius * 0.6f, dpadCy, 270);
        drawDpadArrow(canvas, NativeBridge.VK_RIGHT, dpadCx + dpadRadius * 0.6f, dpadCy, 90);

        // D-pad hub.
        fill.setColor(dpadKeys.isEmpty() ? COL_PAD_FILL : COL_PAD_FILL_ON);
        canvas.drawCircle(dpadCx, dpadCy, dpadDead, fill);

        // Face buttons.
        drawButton(canvas, zBtn, "Z", zOn);
        drawButton(canvas, xBtn, "X", xOn);
        drawButton(canvas, cBtn, "C", cOn);
        drawButton(canvas, shiftBtn, "\u21E7", shiftOn); // ⇧
        drawButton(canvas, escBtn, "ESC", false);
    }

    private void drawDpadArrow(Canvas canvas, int vk, float cx, float cy, float rotDeg) {
        boolean on = dpadKeys.contains(Integer.valueOf(vk));
        fill.setColor(on ? COL_PAD_FILL_ON : COL_PAD_FILL);
        float r = dp(15);
        canvas.save();
        canvas.rotate(rotDeg, cx, cy);
        android.graphics.Path p = new android.graphics.Path();
        p.moveTo(cx, cy - r);
        p.lineTo(cx - r, cy + r * 0.7f);
        p.lineTo(cx + r, cy + r * 0.7f);
        p.close();
        canvas.drawPath(p, fill);
        canvas.restore();
    }

    private void drawButton(Canvas canvas, RectF r, String text, boolean on) {
        float rad = dp(10);
        fill.setColor(on ? COL_BTN_FILL_ON : COL_BTN_FILL);
        canvas.drawRoundRect(r, rad, rad, fill);
        ring.setColor(COL_BTN_RING);
        canvas.drawRoundRect(r, rad, rad, ring);
        float ty = r.centerY() - (label.descent() + label.ascent()) / 2f;
        canvas.drawText(text, r.centerX(), ty, label);
    }

    // ---- Multi-touch input ----
    @Override
    public boolean onTouchEvent(MotionEvent ev) {
        int action = ev.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int idx = ev.getActionIndex();
                handlePointerDown(ev.getPointerId(idx), ev.getX(idx), ev.getY(idx));
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                for (int i = 0; i < ev.getPointerCount(); i++) {
                    handlePointerMove(ev.getPointerId(i), ev.getX(i), ev.getY(i));
                }
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                int idx = ev.getActionIndex();
                handlePointerUp(ev.getPointerId(idx));
                if (action == MotionEvent.ACTION_CANCEL) releaseAll();
                break;
            }
        }
        invalidate();
        return true;
    }

    private Integer hitButton(float x, float y) {
        if (zBtn.contains(x, y)) return NativeBridge.VK_Z;
        if (xBtn.contains(x, y)) return NativeBridge.VK_X;
        if (cBtn.contains(x, y)) return NativeBridge.VK_C;
        if (shiftBtn.contains(x, y)) return NativeBridge.VK_SHIFT;
        if (escBtn.contains(x, y)) return NativeBridge.VK_ESCAPE;
        return null;
    }

    private boolean inDpad(float x, float y) {
        float dx = x - dpadCx, dy = y - dpadCy;
        return Math.sqrt(dx * dx + dy * dy) <= dpadRadius * 1.15f;
    }

    private void handlePointerDown(int id, float x, float y) {
        Integer btn = hitButton(x, y);
        if (btn != null) {
            pointerOwner.put(id, btn);
            pressButton(btn, true);
            return;
        }
        if (inDpad(x, y)) {
            pointerOwner.put(id, OWNER_DPAD);
            updateDpad(x, y);
        }
    }

    private void handlePointerMove(int id, float x, float y) {
        Integer owner = pointerOwner.get(id);
        if (owner == null) return;
        if (owner == OWNER_DPAD) {
            updateDpad(x, y);
        }
        // Face buttons don't track movement (press-and-hold semantics),
        // matching iOS — a finger that started on Z stays Z until lifted.
    }

    private void handlePointerUp(int id) {
        Integer owner = pointerOwner.remove(id);
        if (owner == null) return;
        if (owner == OWNER_DPAD) {
            clearDpad();
        } else {
            pressButton(owner, false);
        }
    }

    private void pressButton(int vk, boolean down) {
        if (down) NativeBridge.keyDown(vk); else NativeBridge.keyUp(vk);
        if (vk == NativeBridge.VK_Z) zOn = down;
        else if (vk == NativeBridge.VK_X) xOn = down;
        else if (vk == NativeBridge.VK_C) cOn = down;
        else if (vk == NativeBridge.VK_SHIFT) shiftOn = down;
    }

    /** Angle-based 8-way D-pad with centre dead zone (mirrors iOS). */
    private void updateDpad(float x, float y) {
        float dx = x - dpadCx, dy = y - dpadCy;
        float r = (float) Math.sqrt(dx * dx + dy * dy);

        ArrayList<Integer> next = new ArrayList<Integer>(2);
        if (r >= dpadDead) {
            // atan2 with screen-y flipped so 0deg = right, 90 = up.
            double angle = Math.toDegrees(Math.atan2(-dy, dx));
            if (angle < 0) angle += 360.0;
            if (angle < 22.5 || angle >= 337.5)      { next.add(NativeBridge.VK_RIGHT); }
            else if (angle < 67.5)  { next.add(NativeBridge.VK_RIGHT); next.add(NativeBridge.VK_UP); }
            else if (angle < 112.5) { next.add(NativeBridge.VK_UP); }
            else if (angle < 157.5) { next.add(NativeBridge.VK_UP); next.add(NativeBridge.VK_LEFT); }
            else if (angle < 202.5) { next.add(NativeBridge.VK_LEFT); }
            else if (angle < 247.5) { next.add(NativeBridge.VK_LEFT); next.add(NativeBridge.VK_DOWN); }
            else if (angle < 292.5) { next.add(NativeBridge.VK_DOWN); }
            else                    { next.add(NativeBridge.VK_DOWN); next.add(NativeBridge.VK_RIGHT); }
        }

        // Release keys no longer held.
        for (int i = 0; i < dpadKeys.size(); i++) {
            int k = dpadKeys.get(i);
            if (!next.contains(Integer.valueOf(k))) NativeBridge.keyUp(k);
        }
        // Press newly held keys.
        for (int i = 0; i < next.size(); i++) {
            int k = next.get(i);
            if (!dpadKeys.contains(Integer.valueOf(k))) NativeBridge.keyDown(k);
        }
        dpadKeys.clear();
        dpadKeys.addAll(next);
    }

    private void clearDpad() {
        for (int i = 0; i < dpadKeys.size(); i++) NativeBridge.keyUp(dpadKeys.get(i));
        dpadKeys.clear();
    }

    private void releaseAll() {
        clearDpad();
        if (zOn) { NativeBridge.keyUp(NativeBridge.VK_Z); zOn = false; }
        if (xOn) { NativeBridge.keyUp(NativeBridge.VK_X); xOn = false; }
        if (cOn) { NativeBridge.keyUp(NativeBridge.VK_C); cOn = false; }
        if (shiftOn) { NativeBridge.keyUp(NativeBridge.VK_SHIFT); shiftOn = false; }
        pointerOwner.clear();
    }

    /** Release everything (call when the game is paused/backgrounded). */
    public void resetInput() { releaseAll(); invalidate(); }
}
