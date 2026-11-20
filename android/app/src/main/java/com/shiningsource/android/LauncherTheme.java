package com.shiningsource.android;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Build;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.IOException;
import java.io.InputStream;

/** Native launcher styling shared by all launcher pages. */
final class LauncherTheme {
    static final int PLAY = 0, VIDEO = 1, ONLINE = 2, SETTINGS = 3;
    static final int INK = 0xff0c1816, CREAM = 0xfff5ebd3;
    static final int BRASS = 0xffcaa564, MUTED = 0xffaebcb0;
    private static final int TEAL_TOP = 0xff25453b, TEAL_BOTTOM = 0xff122a25;
    private static final int BORDER = 0xff596448, FOCUS = 0xffedc57c;
    private static final int DISABLED = 0xff92a39a;
    private static final Typeface TITLE = Typeface.create("serif", Typeface.BOLD);

    private final Context context;
    private final Bitmap grain;

    LauncherTheme(Context context) {
        this.context = context;
        int[] pixels = new int[64 * 64];
        int seed = 0x352bf741;
        for (int i = 0; i < pixels.length; i++) {
            seed = seed * 1664525 + 1013904223;
            int grainValue = seed >>> 28;
            pixels[i] = grainValue < 8 ? grainValue << 24 : ((grainValue - 8) << 24) | 0x00ffffff;
        }
        grain = Bitmap.createBitmap(pixels, 64, 64, Bitmap.Config.ARGB_8888);
    }

    int dp(float value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }

    Drawable background() {
        return surface(0xff1a302b, INK, Color.TRANSPARENT, 0);
    }

    Drawable panel() {
        return surface(0xff213830, 0xff142822, BORDER, 14);
    }

    View hero(String title, String subtitle) {
        LinearLayout hero = new LinearLayout(context);
        hero.setOrientation(LinearLayout.HORIZONTAL);
        hero.setGravity(Gravity.CENTER_VERTICAL);
        hero.setBaselineAligned(false);
        hero.setBackground(panel());
        hero.setPadding(dp(16), dp(14), dp(14), dp(14));

        LinearLayout text = new LinearLayout(context);
        text.setOrientation(LinearLayout.VERTICAL);
        TextView heading = new TextView(context);
        heading.setText(title);
        heading.setTextColor(BRASS);
        heading.setTextSize(19);
        heading.setTypeface(TITLE);
        heading.setMaxLines(1);
        if (Build.VERSION.SDK_INT >= 26)
            heading.setAutoSizeTextTypeUniformWithConfiguration(13, 27, 1, android.util.TypedValue.COMPLEX_UNIT_SP);
        if (Build.VERSION.SDK_INT >= 28) heading.setAccessibilityHeading(true);
        text.addView(heading);
        TextView description = new TextView(context);
        description.setText(subtitle);
        description.setTextColor(CREAM);
        description.setTextSize(13);
        text.addView(description);
        hero.addView(text, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));

        try (InputStream stream = context.getAssets().open("assets/launcher/cover.png")) {
            Bitmap cover = BitmapFactory.decodeStream(stream);
            if (cover != null) {
                ImageView art = new ImageView(context);
                art.setImageBitmap(cover);
                art.setScaleType(ImageView.ScaleType.CENTER_CROP);
                art.setBackground(surface(TEAL_TOP, TEAL_BOTTOM, BORDER, 9));
                art.setClipToOutline(true);
                art.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
                LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(dp(82), dp(82));
                size.setMarginStart(dp(12));
                hero.addView(art, size);
            }
        } catch (IOException ignored) {
            // The title and description are still usable if artwork is unavailable.
        }
        return hero;
    }

    void label(TextView text) {
        text.setTextColor(MUTED);
        text.setTextSize(14);
    }

    void button(Button button, boolean primary) {
        styleButton(button, primary, false);
    }

    void tab(Button button, int icon, boolean selected) {
        styleButton(button, selected, true);
        Glyph glyph = new Glyph(icon, selected ? INK : BRASS);
        glyph.setBounds(0, 0, dp(17), dp(17));
        button.setCompoundDrawablesRelative(null, glyph, null, null);
        button.setCompoundDrawablePadding(dp(3));
    }

    private void styleButton(Button button, boolean primary, boolean tab) {
        int foreground = primary ? INK : CREAM;
        int top = primary ? 0xffd0a563 : TEAL_TOP;
        int bottom = primary ? 0xffab8047 : TEAL_BOTTOM;
        int edge = primary ? FOCUS : BORDER;
        StateListDrawable bg = new StateListDrawable();
        bg.addState(new int[] { -android.R.attr.state_enabled }, surface(0xff263834, 0xff1b2b27, 0xff415249, 9));
        bg.addState(new int[] { android.R.attr.state_pressed }, surface(bottom, top, BRASS, 9));
        bg.addState(new int[] { android.R.attr.state_focused }, surface(top, bottom, 0xffffecb5, 9));
        bg.addState(new int[0], surface(top, bottom, edge, 9));
        button.setBackgroundTintList(null);
        button.setBackground(bg);
        button.setTextColor(new ColorStateList(new int[][] { { -android.R.attr.state_enabled }, {} },
                                               new int[] { DISABLED, foreground }));
        button.setAllCaps(false);
        button.setGravity(Gravity.CENTER);
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setMinHeight(dp(tab ? 54 : 48));
        button.setMinimumHeight(dp(tab ? 54 : 48));
        button.setPadding(dp(tab ? 3 : 12), dp(6), dp(tab ? 3 : 12), dp(6));
        if (tab) {
            button.setTextSize(12);
            button.setMaxLines(2);
        }
    }

    void field(EditText edit) {
        StateListDrawable bg = new StateListDrawable();
        bg.addState(new int[] { android.R.attr.state_focused }, surface(TEAL_TOP, INK, FOCUS, 9));
        bg.addState(new int[0], surface(0xff213830, INK, BORDER, 9));
        edit.setBackgroundTintList(null);
        edit.setBackground(bg);
        edit.setTextColor(CREAM);
        edit.setHintTextColor(MUTED);
        edit.setMinHeight(dp(48));
        edit.setPadding(dp(12), dp(8), dp(12), dp(8));
    }

    void choice(CompoundButton option) {
        option.setTextColor(CREAM);
        option.setMinHeight(dp(48));
        option.setButtonTintList(new ColorStateList(
                new int[][] { { android.R.attr.state_checked }, {} }, new int[] { BRASS, MUTED }));
    }

    private Drawable surface(int top, int bottom, int border, float radiusDp) {
        return new Surface(top, bottom, border, dp(radiusDp));
    }

    private final class Surface extends Drawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rectangle = new RectF();
        private final int top, bottom, border;
        private final float radius;
        private final Shader noise = new BitmapShader(grain, Shader.TileMode.REPEAT, Shader.TileMode.REPEAT);
        private Shader gradient;

        Surface(int top, int bottom, int border, float radius) {
            this.top = top;
            this.bottom = bottom;
            this.border = border;
            this.radius = radius;
        }

        @Override protected void onBoundsChange(Rect bounds) {
            rectangle.set(bounds);
            gradient = new LinearGradient(0, bounds.top, 0, Math.max(bounds.bottom, bounds.top + 1),
                                          top, bottom, Shader.TileMode.CLAMP);
        }

        @Override public void draw(Canvas canvas) {
            paint.setStyle(Paint.Style.FILL);
            paint.setShader(gradient);
            canvas.drawRoundRect(rectangle, radius, radius, paint);
            paint.setShader(noise);
            canvas.drawRoundRect(rectangle, radius, radius, paint);
            paint.setShader(null);
            if (border != Color.TRANSPARENT) {
                float width = dp(1);
                paint.setColor(border);
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(width);
                rectangle.inset(width / 2, width / 2);
                canvas.drawRoundRect(rectangle, radius, radius, paint);
                rectangle.inset(-width / 2, -width / 2);
            }
        }

        @Override public void getOutline(android.graphics.Outline outline) {
            outline.setRoundRect(getBounds(), radius);
        }

        @Override public void setAlpha(int alpha) { paint.setAlpha(alpha); invalidateSelf(); }
        @Override public void setColorFilter(ColorFilter filter) { paint.setColorFilter(filter); invalidateSelf(); }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    private static final class Glyph extends Drawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path outline = new Path();
        private final int kind;

        Glyph(int kind, int color) {
            this.kind = kind;
            paint.setColor(color);
            paint.setStrokeWidth(1.6f);
            paint.setStrokeCap(Paint.Cap.ROUND);
            paint.setStrokeJoin(Paint.Join.ROUND);
        }

        @Override public void draw(Canvas canvas) {
            int save = canvas.save();
            Rect bounds = getBounds();
            canvas.translate(bounds.left, bounds.top);
            canvas.scale(bounds.width() / 18f, bounds.height() / 18f);
            paint.setStyle(Paint.Style.STROKE);
            switch (kind) {
                case PLAY:
                    outline.rewind();
                    outline.moveTo(5, 2); outline.lineTo(15, 9); outline.lineTo(5, 16); outline.close();
                    canvas.drawPath(outline, paint);
                    break;
                case VIDEO:
                    canvas.drawRoundRect(2, 3, 16, 13, 2, 2, paint);
                    canvas.drawLine(6, 16, 12, 16, paint);
                    break;
                case ONLINE:
                    canvas.drawLine(5, 12, 9, 6, paint);
                    canvas.drawLine(9, 6, 13, 12, paint);
                    paint.setStyle(Paint.Style.FILL);
                    canvas.drawCircle(9, 4, 2.2f, paint);
                    canvas.drawCircle(4, 13, 2.2f, paint);
                    canvas.drawCircle(14, 13, 2.2f, paint);
                    break;
                case SETTINGS:
                    canvas.drawLine(3, 4, 15, 4, paint);
                    canvas.drawLine(3, 9, 15, 9, paint);
                    canvas.drawLine(3, 14, 15, 14, paint);
                    canvas.drawLine(7, 2, 7, 6, paint);
                    canvas.drawLine(11, 7, 11, 11, paint);
                    canvas.drawLine(6, 12, 6, 16, paint);
                    break;
            }
            canvas.restoreToCount(save);
        }

        @Override public void setAlpha(int alpha) { paint.setAlpha(alpha); invalidateSelf(); }
        @Override public void setColorFilter(ColorFilter filter) { paint.setColorFilter(filter); invalidateSelf(); }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }
}
