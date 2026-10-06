package com.phantomgo.ui;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RadialGradient;
import android.graphics.Shader;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;

import androidx.annotation.Nullable;

public class JoystickView extends View {

    private Paint bgPaint;
    private Paint knobPaint;
    private Paint borderPaint;

    private float centerX, centerY;
    private float radius;
    private float knobX, knobY;
    private float knobRadius = 0f;

    private boolean active = false;

    public interface OnMoveListener {
        void onMove(float x, float y, boolean isActive);
    }

    private OnMoveListener listener;

    public JoystickView(Context context) {
        super(context);
        init();
    }

    public JoystickView(Context context, @Nullable AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    public JoystickView(Context context, @Nullable AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        init();
    }

    private void init() {
        bgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        bgPaint.setColor(Color.argb(40, 255, 255, 255));

        borderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        borderPaint.setStyle(Paint.Style.STROKE);
        borderPaint.setStrokeWidth(3f);
        borderPaint.setColor(Color.argb(80, 255, 255, 255));

        knobPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        knobPaint.setColor(0xFF22D3EE);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        centerX = w / 2f;
        centerY = h / 2f;
        radius = Math.min(w, h) / 2f - 8f;
        knobRadius = radius * 0.42f;
        knobX = centerX;
        knobY = centerY;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        // Outer circle background
        canvas.drawCircle(centerX, centerY, radius, bgPaint);
        canvas.drawCircle(centerX, centerY, radius, borderPaint);

        // Inner dashed ring
        borderPaint.setStrokeWidth(2f);
        borderPaint.setColor(Color.argb(40, 255, 255, 255));
        canvas.drawCircle(centerX, centerY, radius * 0.7f, borderPaint);
        borderPaint.setStrokeWidth(3f);
        borderPaint.setColor(Color.argb(80, 255, 255, 255));

        // Knob with gradient
        RadialGradient grad = new RadialGradient(
                knobX, knobY, knobRadius,
                new int[]{0xFF67E8F9, 0xFFA855F7},
                null, Shader.TileMode.CLAMP);
        knobPaint.setShader(grad);
        canvas.drawCircle(knobX, knobY, knobRadius, knobPaint);
        knobPaint.setShader(null);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        switch (event.getAction()) {
            case MotionEvent.ACTION_DOWN:
                active = true;
                updateKnob(event.getX(), event.getY());
                return true;
            case MotionEvent.ACTION_MOVE:
                if (active) updateKnob(event.getX(), event.getY());
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                active = false;
                knobX = centerX;
                knobY = centerY;
                invalidate();
                if (listener != null) listener.onMove(0, 0, false);
                return true;
        }
        return super.onTouchEvent(event);
    }

    private void updateKnob(float x, float y) {
        float dx = x - centerX;
        float dy = y - centerY;
        float dist = (float) Math.hypot(dx, dy);
        float maxR = radius - knobRadius * 0.5f;
        if (dist > maxR) {
            dx = dx / dist * maxR;
            dy = dy / dist * maxR;
        }
        knobX = centerX + dx;
        knobY = centerY + dy;
        invalidate();
        if (listener != null) {
            listener.onMove(dx / maxR, dy / maxR, true);
        }
    }

    public void setOnMoveListener(OnMoveListener l) {
        this.listener = l;
    }
}
