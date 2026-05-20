package com.sahilm9098.arkmodmenu;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.PixelFormat;
import android.opengl.GLSurfaceView;
import android.view.Surface;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

@SuppressLint("ViewConstructor")
public final class GLES3JNIView extends GLSurfaceView implements GLSurfaceView.Renderer {
    private final float density;

    public GLES3JNIView(Context context, float density) {
        super(context);
        this.density = density;
        setZOrderOnTop(true);
        setEGLConfigChooser(8, 8, 8, 8, 16, 0);
        getHolder().setFormat(PixelFormat.TRANSLUCENT);
        setEGLContextClientVersion(3);
        setRenderer(this);
        setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
    }

    @Override
    public void onSurfaceCreated(GL10 gl, EGLConfig config) {
        initImgui(getHolder().getSurface(), density);
    }

    @Override
    public void onSurfaceChanged(GL10 gl, int width, int height) {
        updateSize(width, height);
    }

    @Override
    public void onDrawFrame(GL10 gl) {
        Tick();
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        imguiShutdown();
    }

    boolean isMenuVisible() {
        return nativeIsMenuVisible();
    }

    public static native boolean nativeIsMenuVisible();
    public static native void initImgui(Surface surface, float density);
    public static native void updateSize(int width, int height);
    public static native void Tick();
    public static native void imguiShutdown();
    public static native void motionEventClick(boolean down, float posX, float posY);
    public static native String[] getWindowRect();
}
