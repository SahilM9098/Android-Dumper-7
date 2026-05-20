package com.sahilm9098.arkmodmenu;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Context;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;

public final class MenuOverlay {
    private static final String TAG = "MenuOverlay";
    private static final int MAX_WINDOW_AMOUNT = 20;
    private static final int INVALID_VALUE = Integer.MIN_VALUE;

    static Activity mActivity;
    static GLES3JNIView imGuiSurface;

    private static WindowManager windowManager;
    private static int[] windowRegistered;
    private static WindowManager.LayoutParams[] touchHandlerParams;
    private static View[] touchHandlers;
    private static int[] lastHandlerX;
    private static int[] lastHandlerY;
    private static int[] lastHandlerW;
    private static int[] lastHandlerH;
    private static boolean touchGestureActive;
    private static int activeHandlerIndex = -1;
    private static int touchLogCount;
    private static final Handler touchHandler = new Handler(Looper.getMainLooper());
    private static Runnable touchUpdater;

    private MenuOverlay() {}

    public static void Start(Context context) {
        if (!(context instanceof Activity)) {
            return;
        }

        Activity activity = (Activity) context;
        if (mActivity == activity && imGuiSurface != null && imGuiSurface.getParent() != null) {
            resumeExistingOverlay(activity);
            return;
        }

        if (mActivity != null || imGuiSurface != null) {
            Stop();
        }

        mActivity = activity;
        windowManager = (WindowManager) activity.getSystemService(Context.WINDOW_SERVICE);
        float density = activity.getResources().getDisplayMetrics().density;
        imGuiSurface = new GLES3JNIView(activity, density);

        windowRegistered = new int[MAX_WINDOW_AMOUNT];
        touchHandlerParams = new WindowManager.LayoutParams[MAX_WINDOW_AMOUNT];
        touchHandlers = new View[MAX_WINDOW_AMOUNT];
        lastHandlerX = new int[MAX_WINDOW_AMOUNT];
        lastHandlerY = new int[MAX_WINDOW_AMOUNT];
        lastHandlerW = new int[MAX_WINDOW_AMOUNT];
        lastHandlerH = new int[MAX_WINDOW_AMOUNT];

        for (int i = 0; i < MAX_WINDOW_AMOUNT; i++) {
            lastHandlerX[i] = INVALID_VALUE;
            lastHandlerY[i] = INVALID_VALUE;
            lastHandlerW[i] = INVALID_VALUE;
            lastHandlerH[i] = INVALID_VALUE;
        }

        windowManager.addView(imGuiSurface, getAttributes(true));
        Log.i(TAG, "started on " + activity.getClass().getName());
        startTouchUpdater(activity);
    }

    public static void Resume(Activity activity) {
        if (activity == null) {
            return;
        }
        Start(activity);
    }

    public static void Pause(Activity activity) {
        if (mActivity == activity && imGuiSurface != null) {
            imGuiSurface.onPause();
            Log.i(TAG, "paused");
        }
    }

    public static void Stop() {
        touchGestureActive = false;
        activeHandlerIndex = -1;
        touchLogCount = 0;

        if (touchUpdater != null) {
            touchHandler.removeCallbacks(touchUpdater);
            touchUpdater = null;
        }

        if (touchHandlers != null) {
            for (int i = 0; i < touchHandlers.length; i++) {
                removeViewQuietly(touchHandlers[i]);
                touchHandlers[i] = null;
                if (touchHandlerParams != null) {
                    touchHandlerParams[i] = null;
                }
                if (windowRegistered != null) {
                    windowRegistered[i] = 0;
                }
            }
        }

        removeViewQuietly(imGuiSurface);
        imGuiSurface = null;
        windowManager = null;
        mActivity = null;
    }

    public static boolean dispatchKeyEvent(android.view.KeyEvent event) {
        return false;
    }

    public static void onBackPressed() {}

    private static void resumeExistingOverlay(final Activity activity) {
        imGuiSurface.setVisibility(View.VISIBLE);
        imGuiSurface.onResume();
        imGuiSurface.requestRender();

        if (touchUpdater == null) {
            startTouchUpdater(activity);
        }

        try {
            windowManager.updateViewLayout(imGuiSurface, getAttributes(true));
        } catch (RuntimeException ignored) {
            Log.w(TAG, "overlay refresh failed, recreating", ignored);
            Stop();
            Start(activity);
            return;
        }

        Log.i(TAG, "resumed existing overlay");
    }

    private static void startTouchUpdater(final Context context) {
        touchUpdater = new Runnable() {
            @Override
            public void run() {
                if (mActivity == null || imGuiSurface == null || windowManager == null) {
                    return;
                }

                updateTouchHandlers(context);
                touchHandler.postDelayed(this, 16);
            }
        };
        touchHandler.postDelayed(touchUpdater, 500);
    }

    private static void updateTouchHandlers(Context context) {
        String[] windows = GLES3JNIView.getWindowRect();
        if (windows == null) {
            return;
        }

        for (int i = 0; i < windows.length && i < MAX_WINDOW_AMOUNT; i++) {
            String window = windows[i];
            if (window == null || window.isEmpty()) {
                removeTouchHandler(i);
                continue;
            }

            String[] fields = window.split("\\|");
            if (fields.length < 5 || "1000".equals(fields[0])) {
                removeTouchHandler(i);
                continue;
            }

            try {
                int id = (int) Float.parseFloat(fields[0]);
                int x = Math.round(Float.parseFloat(fields[1]));
                int y = Math.round(Float.parseFloat(fields[2]));
                int width = Math.max(1, Math.round(Float.parseFloat(fields[3])));
                int height = Math.max(1, Math.round(Float.parseFloat(fields[4])));

                if (touchHandlers[i] == null) {
                    createTouchHandler(context, i, id);
                }

                if (touchGestureActive && activeHandlerIndex == i) {
                    continue;
                }

                WindowManager.LayoutParams params = touchHandlerParams[i];
                if (params == null || touchHandlers[i] == null) {
                    continue;
                }

                if (lastHandlerX[i] == x && lastHandlerY[i] == y
                        && lastHandlerW[i] == width && lastHandlerH[i] == height) {
                    continue;
                }

                params.x = x;
                params.y = y;
                params.width = width;
                params.height = height;
                windowRegistered[i] = id;
                lastHandlerX[i] = x;
                lastHandlerY[i] = y;
                lastHandlerW[i] = width;
                lastHandlerH[i] = height;
                windowManager.updateViewLayout(touchHandlers[i], params);
                if (i == 0) {
                    Log.i(TAG, "touch window[0] rect x=" + x + " y=" + y
                            + " w=" + width + " h=" + height);
                }
            } catch (RuntimeException ignored) {
                Log.w(TAG, "touch window update failed at index " + i, ignored);
                removeTouchHandler(i);
            }
        }
    }

    private static void createTouchHandler(Context context, final int index, int id) {
        View view = new View(context);
        view.setOnTouchListener(new View.OnTouchListener() {
            @SuppressLint("ClickableViewAccessibility")
            @Override
            public boolean onTouch(View v, MotionEvent event) {
                WindowManager.LayoutParams params = touchHandlerParams[index];
                float localX = event.getX() + (params != null ? params.x : 0);
                float localY = event.getY() + (params != null ? params.y : 0);
                int action = event.getActionMasked();
                boolean down = action != MotionEvent.ACTION_UP
                        && action != MotionEvent.ACTION_CANCEL;

                if (action == MotionEvent.ACTION_DOWN) {
                    touchGestureActive = true;
                    activeHandlerIndex = index;
                    if (touchLogCount < 20) {
                        Log.i(TAG, "touch down index=" + index
                                + " x=" + localX + " y=" + localY);
                        touchLogCount++;
                    }
                }

                GLES3JNIView.motionEventClick(down, localX, localY);
                if (imGuiSurface != null) {
                    imGuiSurface.requestRender();
                }

                if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
                    if (touchLogCount < 20) {
                        Log.i(TAG, "touch up index=" + index
                                + " x=" + localX + " y=" + localY);
                        touchLogCount++;
                    }
                    touchGestureActive = false;
                    activeHandlerIndex = -1;
                }
                return true;
            }
        });

        windowRegistered[index] = id;
        touchHandlerParams[index] = getAttributes(false);
        touchHandlers[index] = view;
        windowManager.addView(view, touchHandlerParams[index]);
        Log.i(TAG, "created touch handler index=" + index + " id=" + id);
    }

    private static void removeTouchHandler(int index) {
        if (touchHandlers == null || index < 0 || index >= touchHandlers.length) {
            return;
        }

        removeViewQuietly(touchHandlers[index]);
        touchHandlers[index] = null;

        if (touchHandlerParams != null) {
            touchHandlerParams[index] = null;
        }
        if (windowRegistered != null) {
            windowRegistered[index] = 0;
        }
        if (lastHandlerX != null) lastHandlerX[index] = INVALID_VALUE;
        if (lastHandlerY != null) lastHandlerY[index] = INVALID_VALUE;
        if (lastHandlerW != null) lastHandlerW[index] = INVALID_VALUE;
        if (lastHandlerH != null) lastHandlerH[index] = INVALID_VALUE;
    }

    private static void removeViewQuietly(View view) {
        if (view == null || windowManager == null) {
            return;
        }

        try {
            windowManager.removeViewImmediate(view);
        } catch (RuntimeException ignored) {
        }
    }

    private static WindowManager.LayoutParams getAttributes(boolean isWindow) {
        WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                isWindow ? WindowManager.LayoutParams.MATCH_PARENT : 0,
                isWindow ? WindowManager.LayoutParams.MATCH_PARENT : 0,
                WindowManager.LayoutParams.TYPE_APPLICATION,
                WindowManager.LayoutParams.FLAG_FULLSCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_TRANSLUCENT_STATUS
                        | WindowManager.LayoutParams.FLAG_TRANSLUCENT_NAVIGATION
                        | WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
                PixelFormat.RGBA_8888);

        if (isWindow) {
            params.flags |= WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                    | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL;
        } else {
            params.flags |= WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            params.layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }

        params.gravity = Gravity.LEFT | Gravity.TOP;
        params.x = 0;
        params.y = 0;
        applyActivityToken(params);
        return params;
    }

    private static void applyActivityToken(WindowManager.LayoutParams params) {
        if (params == null || mActivity == null || mActivity.getWindow() == null) {
            return;
        }

        params.token = mActivity.getWindow().getAttributes().token;
        if (params.token == null && mActivity.getWindow().getDecorView() != null) {
            params.token = mActivity.getWindow().getDecorView().getApplicationWindowToken();
        }
    }
}
