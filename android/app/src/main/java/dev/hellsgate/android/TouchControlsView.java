package dev.hellsgate.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

import org.libsdl.app.TouchBridge;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * On-screen Xbox 360 style controller. It registers itself with SDL as an Android gamepad
 * (SDLControllerManager.nativeAddJoystick via TouchBridge) and sends button/axis events through the same JNI
 * entry points a physical controller uses, so the game just sees a normal gamepad.
 */
public class TouchControlsView extends View {
    // Android key codes (android.view.KeyEvent) that SDL maps to gamepad buttons.
    private static final int KEY_A = 96, KEY_B = 97, KEY_X = 99, KEY_Y = 100;
    private static final int KEY_L1 = 102, KEY_R1 = 103, KEY_THUMBL = 106, KEY_THUMBR = 107;
    private static final int KEY_START = 108, KEY_SELECT = 109;
    private static final int KEY_DPAD_UP = 19, KEY_DPAD_DOWN = 20, KEY_DPAD_LEFT = 21, KEY_DPAD_RIGHT = 22;

    // SDL axis indices of the auto-generated Android mapping (leftx..righttrigger = a0..a5).
    private static final int AXIS_LX = 0, AXIS_LY = 1, AXIS_RX = 2, AXIS_RY = 3, AXIS_LT = 4, AXIS_RT = 5;

    static final int DEVICE_ID = 0x7E57C0DE;
    private static boolean registered;

    private static final String PREFS = "touch_controls";

    private abstract class Control {
        final String label;
        float cx, cy, r;        // layout, pixels
        int pointer = -1;
        Control(String label) { this.label = label; }
        boolean hit(float x, float y) {
            float dx = x - cx, dy = y - cy;
            return dx * dx + dy * dy <= r * r * 1.35f;
        }
        abstract void down(float x, float y);
        void move(float x, float y) {}
        abstract void up();
        abstract void draw(Canvas c);
    }

    private class Button extends Control {
        final int key;      // key code, or -1 for a trigger
        final int axis;     // trigger axis when key == -1
        boolean pressed;
        Button(String label, int key) { super(label); this.key = key; this.axis = -1; }
        Button(String label, int axis, boolean trigger) { super(label); this.key = -1; this.axis = axis; }
        @Override void down(float x, float y) {
            pressed = true;
            if (key >= 0) TouchBridge.buttonDown(DEVICE_ID, key);
            else TouchBridge.axis(DEVICE_ID, axis, 1.0f);
            performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        }
        @Override void up() {
            pressed = false;
            if (key >= 0) TouchBridge.buttonUp(DEVICE_ID, key);
            else TouchBridge.axis(DEVICE_ID, axis, -1.0f);
        }
        @Override void draw(Canvas c) {
            fill.setColor(pressed ? 0x99FFFFFF : 0x40FFFFFF);
            c.drawCircle(cx, cy, r, fill);
            c.drawCircle(cx, cy, r, stroke);
            text.setTextSize(r * (label.length() > 2 ? 0.55f : 0.8f));
            c.drawText(label, cx, cy - (text.descent() + text.ascent()) / 2, text);
        }
    }

    private class Stick extends Control {
        final int axisX, axisY;
        float kx, ky;           // knob offset, pixels
        float ox, oy;           // origin of the current touch (floating stick)
        final float homeX, homeY;
        Stick(String label, int axisX, int axisY, float x, float y) {
            super(label); this.axisX = axisX; this.axisY = axisY; homeX = x; homeY = y;
        }
        @Override boolean hit(float x, float y) {
            float dx = x - cx, dy = y - cy;
            return dx * dx + dy * dy <= r * r * 2.2f;
        }
        @Override void down(float x, float y) {
            // Re-centre the stick where the thumb lands (within the zone) so it never starts tilted.
            ox = x; oy = y; cx = x; cy = y;
            kx = ky = 0;
            send();
        }
        @Override void move(float x, float y) {
            float dx = x - ox, dy = y - oy;
            float len = (float) Math.hypot(dx, dy);
            if (len > r) { dx = dx / len * r; dy = dy / len * r; }
            kx = dx; ky = dy;
            send();
        }
        @Override void up() {
            kx = ky = 0;
            cx = homeX * getWidth(); cy = homeY * getHeight();
            send();
        }
        private void send() {
            float vx = kx / r, vy = ky / r;
            TouchBridge.axis(DEVICE_ID, axisX, clamp(vx));
            TouchBridge.axis(DEVICE_ID, axisY, clamp(vy));
        }
        @Override void draw(Canvas c) {
            fill.setColor(0x30FFFFFF);
            c.drawCircle(cx, cy, r, fill);
            c.drawCircle(cx, cy, r, stroke);
            fill.setColor(pointer >= 0 ? 0x99FFFFFF : 0x60FFFFFF);
            c.drawCircle(cx + kx, cy + ky, r * 0.45f, fill);
            text.setTextSize(r * 0.3f);
            c.drawText(label, cx + kx, cy + ky - (text.descent() + text.ascent()) / 2, text);
        }
    }

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final List<Control> controls = new ArrayList<>();
    private final Map<Integer, Control> active = new HashMap<>();
    private Stick leftStick, rightStick;
    private final RectF toggleRect = new RectF();
    private boolean visible = true;

    public TouchControlsView(Context context) {
        super(context);
        fill.setStyle(Paint.Style.FILL);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setColor(0x80FFFFFF);
        stroke.setStrokeWidth(3);
        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        SharedPreferences p = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        visible = p.getBoolean("visible", true);
        setFocusable(false);
    }

    /** Registers the virtual gamepad with SDL. Safe to call more than once. */
    static synchronized void registerGamepad() {
        if (registered) return;
        // a,b,x,y,back,start,l3,r3,lb,rb,dpad (SDL gamepad button bits 0-14, without guide).
        int buttonMask = 0x7FFF & ~(1 << 5);
        int axisMask = 0x3F;   // left x/y, right x/y, left/right trigger
        TouchBridge.addJoystick(DEVICE_ID, "Touch Controller", "hellsgate-touch",
                0, 0, buttonMask, 6, axisMask);  // vendor 0: not claimed by the HIDAPI drivers
        // Triggers rest at -1 (released) on a full-range axis.
        TouchBridge.axis(DEVICE_ID, AXIS_LT, -1.0f);
        TouchBridge.axis(DEVICE_ID, AXIS_RT, -1.0f);
        registered = true;
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        layoutControls(w, h);
    }

    private void layoutControls(int w, int h) {
        releaseAll();
        controls.clear();
        float u = Math.min(w, h);          // short side
        float face = u * 0.075f;           // face button radius
        float small = u * 0.06f;

        // Face buttons (Xbox layout), lower right.
        float fx = w - u * 0.24f, fy = h - u * 0.28f, sp = face * 1.55f;
        controls.add(place(new Button("A", KEY_A), fx, fy + sp, face));
        controls.add(place(new Button("B", KEY_B), fx + sp, fy, face));
        controls.add(place(new Button("X", KEY_X), fx - sp, fy, face));
        controls.add(place(new Button("Y", KEY_Y), fx, fy - sp, face));

        // Shoulders and triggers, top corners.
        controls.add(place(new Button("LT", AXIS_LT, true), u * 0.12f, u * 0.10f, small));
        controls.add(place(new Button("LB", KEY_L1), u * 0.30f, u * 0.10f, small));
        controls.add(place(new Button("RB", KEY_R1), w - u * 0.30f, u * 0.10f, small));
        controls.add(place(new Button("RT", AXIS_RT, true), w - u * 0.12f, u * 0.10f, small));

        // Back / Start, top centre.
        controls.add(place(new Button("BACK", KEY_SELECT), w * 0.5f - u * 0.11f, u * 0.08f, small * 0.8f));
        controls.add(place(new Button("START", KEY_START), w * 0.5f + u * 0.11f, u * 0.08f, small * 0.8f));

        // D-pad, left middle.
        float dx = u * 0.20f, dy = h * 0.42f, ds = small * 1.25f;
        controls.add(place(new Button("▲", KEY_DPAD_UP), dx, dy - ds, small * 0.75f));
        controls.add(place(new Button("▼", KEY_DPAD_DOWN), dx, dy + ds, small * 0.75f));
        controls.add(place(new Button("◀", KEY_DPAD_LEFT), dx - ds, dy, small * 0.75f));
        controls.add(place(new Button("▶", KEY_DPAD_RIGHT), dx + ds, dy, small * 0.75f));

        // Sticks: left stick lower left, right stick left of the face buttons.
        float stickR = u * 0.13f;
        leftStick = new Stick("L", AXIS_LX, AXIS_LY, (u * 0.26f) / w, (h - u * 0.24f) / h);
        place(leftStick, u * 0.26f, h - u * 0.24f, stickR);
        controls.add(leftStick);
        rightStick = new Stick("R", AXIS_RX, AXIS_RY, (w - u * 0.62f) / w, (h - u * 0.20f) / h);
        place(rightStick, w - u * 0.62f, h - u * 0.20f, stickR * 0.8f);
        controls.add(rightStick);

        // Stick clicks.
        controls.add(place(new Button("L3", KEY_THUMBL), u * 0.50f, h - u * 0.08f, small * 0.7f));
        controls.add(place(new Button("R3", KEY_THUMBR), w - u * 0.80f, h - u * 0.08f, small * 0.7f));

        // Show/hide toggle, top centre.
        float t = small * 0.7f;
        toggleRect.set(w * 0.5f - t, u * 0.20f - t * 0.6f, w * 0.5f + t, u * 0.20f + t * 0.6f);
        invalidate();
    }

    private Control place(Control c, float x, float y, float r) {
        c.cx = x; c.cy = y; c.r = r;
        return c;
    }

    private static float clamp(float v) {
        return Math.max(-1f, Math.min(1f, v));
    }

    @Override
    protected void onDraw(Canvas canvas) {
        // Toggle is always drawn, faintly.
        fill.setColor(0x30FFFFFF);
        canvas.drawRoundRect(toggleRect, 12, 12, fill);
        text.setTextSize(toggleRect.height() * 0.45f);
        canvas.drawText(visible ? "hide" : "pad", toggleRect.centerX(),
                toggleRect.centerY() - (text.descent() + text.ascent()) / 2, text);
        if (!visible) return;
        for (Control c : controls) c.draw(canvas);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        registerGamepad();
        int action = e.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex();
                int id = e.getPointerId(i);
                float x = e.getX(i), y = e.getY(i);
                if (toggleRect.contains(x, y)) {
                    visible = !visible;
                    if (!visible) releaseAll();
                    getContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                            .putBoolean("visible", visible).apply();
                    invalidate();
                    return true;
                }
                if (!visible) return false;
                Control hit = null;
                for (Control c : controls) {
                    if (c.pointer < 0 && !(c instanceof Stick) && c.hit(x, y)) { hit = c; break; }
                }
                if (hit == null) {
                    for (Control c : controls) {
                        if (c.pointer < 0 && c instanceof Stick && c.hit(x, y)) { hit = c; break; }
                    }
                }
                if (hit == null && leftStick.pointer < 0 && x < getWidth() * 0.4f
                        && y > getHeight() * 0.5f) {
                    hit = leftStick;   // anywhere in the lower-left area moves the left stick
                }
                if (hit != null) {
                    hit.pointer = id;
                    active.put(id, hit);
                    hit.down(x, y);
                    invalidate();
                }
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                for (int i = 0; i < e.getPointerCount(); i++) {
                    Control c = active.get(e.getPointerId(i));
                    if (c != null) c.move(e.getX(i), e.getY(i));
                }
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int id = e.getPointerId(e.getActionIndex());
                Control c = active.remove(id);
                if (c != null) {
                    c.pointer = -1;
                    c.up();
                    invalidate();
                }
                return true;
            }
            case MotionEvent.ACTION_CANCEL:
                releaseAll();
                invalidate();
                return true;
            default:
                return true;
        }
    }

    private void releaseAll() {
        for (Control c : active.values()) {
            c.pointer = -1;
            c.up();
        }
        active.clear();
    }
}
