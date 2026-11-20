package com.snowboardkids.recomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.SparseArray;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

/**
 * On-screen N64 controller drawn over the game. Touches drive an SDL virtual game controller (see
 * sk1android::set_touch_button / set_touch_axis), so the game sees them like a physical gamepad with the
 * default bindings: A=south, B=west, Z=left trigger, stick=left stick, C-left=north, C-right=east,
 * C-up=right stick click, C-down=right shoulder. Snowboard Kids does not use L or R, so they are left out.
 *
 * Only touches that land on a control are consumed; the parent layout splits multi-touch, so any other
 * touch still reaches the game surface underneath.
 */
public class SK1TouchControls extends View {
    // SDL_GameControllerButton values.
    private static final int BTN_A = 0, BTN_B = 1, BTN_X = 2, BTN_Y = 3, BTN_START = 6;
    private static final int BTN_RIGHTSTICK = 8, BTN_RIGHTSHOULDER = 10;
    // SDL_GameControllerAxis values.
    private static final int AXIS_LEFTX = 0, AXIS_LEFTY = 1, AXIS_TRIGGERLEFT = 4;

    private enum Kind { BUTTON, TRIGGER, STICK, TOGGLE }

    private static final class Control {
        final Kind kind;
        final String label;
        final int target;        // SDL button or axis
        final float cx, cy, r;   // centre as fractions of width/height, radius as a fraction of height
        final boolean pill;      // drawn as a rounded rectangle (shoulder buttons)
        boolean pressed;
        float px, py;            // pixel centre / radius, updated on layout
        float pr;
        float stickX, stickY;    // current stick deflection in [-1, 1]

        Control(Kind kind, String label, int target, float cx, float cy, float r, boolean pill) {
            this.kind = kind;
            this.label = label;
            this.target = target;
            this.cx = cx;
            this.cy = cy;
            this.r = r;
            this.pill = pill;
        }

        boolean hit(float x, float y) {
            float slop = (kind == Kind.STICK) ? 1.35f : 1.15f;
            if (pill) {
                return Math.abs(x - px) <= pr * 1.6f * slop && Math.abs(y - py) <= pr * 0.8f * slop;
            }
            float dx = x - px, dy = y - py;
            return dx * dx + dy * dy <= (pr * slop) * (pr * slop);
        }
    }

    private final List<Control> controls = new ArrayList<>();
    private final SparseArray<Control> pointerControls = new SparseArray<>();
    private final Control toggle;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint outline = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private boolean controlsHidden = false;

    public SK1TouchControls(Context context) {
        super(context);
        // Snowboard Kids uses the stick, A, B, Z, the C-buttons and Start.
        controls.add(new Control(Kind.STICK, "", AXIS_LEFTX, 0.17f, 0.68f, 0.20f, false));
        controls.add(new Control(Kind.BUTTON, "A", BTN_A, 0.91f, 0.74f, 0.10f, false));
        controls.add(new Control(Kind.BUTTON, "B", BTN_X, 0.785f, 0.86f, 0.085f, false));
        controls.add(new Control(Kind.TRIGGER, "Z", AXIS_TRIGGERLEFT, 0.80f, 0.56f, 0.08f, false));
        // C-buttons (board grabs in the air): compact diamond in the upper right, above A.
        float cX = 0.895f, cY = 0.38f, cOffY = 0.11f, cOffX = 0.055f, cR = 0.05f;
        controls.add(new Control(Kind.BUTTON, "C▲", BTN_RIGHTSTICK, cX, cY - cOffY, cR, false));
        controls.add(new Control(Kind.BUTTON, "C▼", BTN_RIGHTSHOULDER, cX, cY + cOffY, cR, false));
        controls.add(new Control(Kind.BUTTON, "C◀", BTN_Y, cX - cOffX, cY, cR, false));
        controls.add(new Control(Kind.BUTTON, "C▶", BTN_B, cX + cOffX, cY, cR, false));
        // Centre bottom: Start.
        controls.add(new Control(Kind.BUTTON, "START", BTN_START, 0.50f, 0.92f, 0.055f, true));
        // Top centre: hide/show toggle (always visible).
        toggle = new Control(Kind.TOGGLE, "", -1, 0.50f, 0.06f, 0.045f, false);

        stroke.setStyle(Paint.Style.STROKE);
        outline.setStyle(Paint.Style.STROKE);
        outline.setColor(0x70000000);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        text.setShadowLayer(4f, 0f, 0f, 0xC0000000);
        setLayerType(View.LAYER_TYPE_SOFTWARE, null); // text shadows need software rendering on some devices
        setHapticFeedbackEnabled(true);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        for (Control c : controls) {
            layout(c, w, h);
        }
        layout(toggle, w, h);
        stroke.setStrokeWidth(Math.max(2f, h * 0.004f));
        outline.setStrokeWidth(Math.max(5f, h * 0.011f));
    }

    private static void layout(Control c, int w, int h) {
        c.px = c.cx * w;
        c.py = c.cy * h;
        c.pr = c.r * h;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if (!controlsHidden) {
            for (Control c : controls) {
                drawControl(canvas, c);
            }
        }
        drawToggle(canvas);
    }

    private void drawControl(Canvas canvas, Control c) {
        fill.setColor(c.pressed ? 0x70FFFFFF : 0x40000000);
        stroke.setColor(0x90FFFFFF);
        if (c.kind == Kind.STICK) {
            canvas.drawCircle(c.px, c.py, c.pr, fill);
            canvas.drawCircle(c.px, c.py, c.pr, outline);
            canvas.drawCircle(c.px, c.py, c.pr, stroke);
            float knobX = c.px + c.stickX * c.pr * 0.6f;
            float knobY = c.py + c.stickY * c.pr * 0.6f;
            fill.setColor((c.pressed ? 0xA0 : 0x60) << 24 | 0xFFFFFF);
            canvas.drawCircle(knobX, knobY, c.pr * 0.42f, fill);
            return;
        }
        if (c.pill) {
            rect.set(c.px - c.pr * 1.6f, c.py - c.pr * 0.8f, c.px + c.pr * 1.6f, c.py + c.pr * 0.8f);
            canvas.drawRoundRect(rect, c.pr * 0.8f, c.pr * 0.8f, fill);
            canvas.drawRoundRect(rect, c.pr * 0.8f, c.pr * 0.8f, outline);
            canvas.drawRoundRect(rect, c.pr * 0.8f, c.pr * 0.8f, stroke);
        } else {
            canvas.drawCircle(c.px, c.py, c.pr, fill);
            canvas.drawCircle(c.px, c.py, c.pr, outline);
            canvas.drawCircle(c.px, c.py, c.pr, stroke);
        }
        text.setColor(0xF0FFFFFF);
        text.setTextSize(c.pr * (c.label.length() > 2 ? 0.55f : 0.9f));
        canvas.drawText(c.label, c.px, c.py - (text.descent() + text.ascent()) / 2f, text);
    }

    private void drawToggle(Canvas canvas) {
        fill.setColor(toggle.pressed ? 0x70FFFFFF : 0x40000000);
        stroke.setColor(0x90FFFFFF);
        canvas.drawCircle(toggle.px, toggle.py, toggle.pr, fill);
        canvas.drawCircle(toggle.px, toggle.py, toggle.pr, outline);
        canvas.drawCircle(toggle.px, toggle.py, toggle.pr, stroke);
        // A small gamepad glyph: crossed out when the controls are hidden.
        float s = toggle.pr * 0.5f;
        rect.set(toggle.px - s, toggle.py - s * 0.6f, toggle.px + s, toggle.py + s * 0.6f);
        canvas.drawRoundRect(rect, s * 0.4f, s * 0.4f, stroke);
        if (controlsHidden) {
            canvas.drawLine(toggle.px - s, toggle.py + s, toggle.px + s, toggle.py - s, stroke);
        }
    }

    private Control findControl(float x, float y) {
        if (toggle.hit(x, y)) {
            return toggle;
        }
        if (controlsHidden) {
            return null;
        }
        for (Control c : controls) {
            if (c.hit(x, y)) {
                return c;
            }
        }
        return null;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        final int action = event.getActionMasked();
        final int index = event.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                Control c = findControl(event.getX(index), event.getY(index));
                if (c == null || (c.pressed && c.kind != Kind.STICK)) {
                    // Not ours: let the game surface receive this pointer.
                    return action != MotionEvent.ACTION_DOWN;
                }
                pointerControls.put(event.getPointerId(index), c);
                press(c, event.getX(index), event.getY(index));
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                for (int i = 0; i < event.getPointerCount(); i++) {
                    Control c = pointerControls.get(event.getPointerId(i));
                    if (c != null && c.kind == Kind.STICK) {
                        updateStick(c, event.getX(i), event.getY(i));
                    }
                }
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int id = event.getPointerId(index);
                Control c = pointerControls.get(id);
                pointerControls.remove(id);
                if (c != null) {
                    release(c);
                }
                return true;
            }
            case MotionEvent.ACTION_CANCEL: {
                for (int i = 0; i < pointerControls.size(); i++) {
                    Control c = pointerControls.valueAt(i);
                    if (c.kind == Kind.TOGGLE) {
                        c.pressed = false; // a cancelled gesture must not flip the toggle
                    } else {
                        release(c);
                    }
                }
                invalidate();
                pointerControls.clear();
                return true;
            }
            default:
                return true;
        }
    }

    private void press(Control c, float x, float y) {
        c.pressed = true;
        switch (c.kind) {
            case BUTTON:
                SK1NativeBridge.nativeTouchButton(c.target, true);
                break;
            case TRIGGER:
                SK1NativeBridge.nativeTouchAxis(c.target, 32767);
                break;
            case STICK:
                updateStick(c, x, y);
                break;
            case TOGGLE:
                break;
        }
        if (c.kind != Kind.STICK) {
            performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        }
        invalidate();
    }

    private void release(Control c) {
        c.pressed = false;
        switch (c.kind) {
            case BUTTON:
                SK1NativeBridge.nativeTouchButton(c.target, false);
                break;
            case TRIGGER:
                SK1NativeBridge.nativeTouchAxis(c.target, -32768);
                break;
            case STICK:
                c.stickX = 0f;
                c.stickY = 0f;
                SK1NativeBridge.nativeTouchAxis(AXIS_LEFTX, 0);
                SK1NativeBridge.nativeTouchAxis(AXIS_LEFTY, 0);
                break;
            case TOGGLE:
                setControlsHidden(!controlsHidden);
                break;
        }
        invalidate();
    }

    private void updateStick(Control c, float x, float y) {
        // Full deflection at 60% of the base radius, so the thumb does not need to reach the rim.
        float range = c.pr * 0.6f;
        float dx = (x - c.px) / range;
        float dy = (y - c.py) / range;
        float len = (float) Math.sqrt(dx * dx + dy * dy);
        if (len > 1f) {
            dx /= len;
            dy /= len;
        }
        c.stickX = dx;
        c.stickY = dy;
        SK1NativeBridge.nativeTouchAxis(AXIS_LEFTX, Math.round(dx * 32767f));
        SK1NativeBridge.nativeTouchAxis(AXIS_LEFTY, Math.round(dy * 32767f));
        invalidate();
    }

    /** Hides the controls (leaving only the toggle) and releases anything still held. */
    public void setControlsHidden(boolean hidden) {
        if (hidden) {
            for (Control c : controls) {
                if (c.pressed) {
                    release(c);
                }
            }
        }
        controlsHidden = hidden;
        invalidate();
    }
}
