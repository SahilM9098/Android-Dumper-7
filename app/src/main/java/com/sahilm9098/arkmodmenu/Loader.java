package com.sahilm9098.arkmodmenu;

import android.app.Activity;
import android.app.Application;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

import java.lang.reflect.Field;
import java.util.Collection;
import java.util.Map;

public final class Loader {
    private static volatile boolean started;

    private Loader() {}

    public static void main() {
        if (started) {
            return;
        }
        started = true;

        new Thread(new Runnable() {
            @Override
            public void run() {
                Application app = waitForApplication();
                if (app == null) {
                    return;
                }

                app.registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
                    @Override
                    public void onActivityCreated(Activity activity, Bundle savedInstanceState) {}

                    @Override
                    public void onActivityStarted(Activity activity) {
                        MenuOverlay.Resume(activity);
                    }

                    @Override
                    public void onActivityResumed(Activity activity) {
                        MenuOverlay.Resume(activity);
                    }

                    @Override
                    public void onActivityPaused(Activity activity) {}

                    @Override
                    public void onActivityStopped(Activity activity) {
                        MenuOverlay.Pause(activity);
                    }

                    @Override
                    public void onActivitySaveInstanceState(Activity activity, Bundle outState) {}

                    @Override
                    public void onActivityDestroyed(Activity activity) {
                        if (MenuOverlay.mActivity == activity) {
                            MenuOverlay.Stop();
                        }
                    }
                });

                final Activity current = fetchCurrentActivity();
                if (current != null) {
                    new Handler(Looper.getMainLooper()).post(new Runnable() {
                        @Override
                        public void run() {
                            MenuOverlay.Start(current);
                        }
                    });
                }
            }
        }).start();
    }

    private static Application waitForApplication() {
        Application app;
        do {
            app = fetchApplication();
            if (app == null) {
                try {
                    Thread.sleep(50);
                } catch (InterruptedException ignored) {
                    Thread.currentThread().interrupt();
                    return null;
                }
            }
        } while (app == null);
        return app;
    }

    private static Application fetchApplication() {
        try {
            Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
            Object activityThread = activityThreadClass
                    .getMethod("currentActivityThread")
                    .invoke(null);

            if (activityThread != null) {
                return (Application) activityThreadClass
                        .getMethod("getApplication")
                        .invoke(activityThread);
            }

            return (Application) activityThreadClass
                    .getMethod("currentApplication")
                    .invoke(null);
        } catch (ReflectiveOperationException ignored) {
            return null;
        }
    }

    private static Activity fetchCurrentActivity() {
        try {
            Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
            Object activityThread = activityThreadClass
                    .getMethod("currentActivityThread")
                    .invoke(null);
            if (activityThread == null) {
                return null;
            }

            Field activitiesField = activityThreadClass.getDeclaredField("mActivities");
            activitiesField.setAccessible(true);
            Object activities = activitiesField.get(activityThread);
            if (!(activities instanceof Map)) {
                return null;
            }

            Collection<?> records = ((Map<?, ?>) activities).values();
            for (Object record : records) {
                Class<?> recordClass = record.getClass();
                Field pausedField = recordClass.getDeclaredField("paused");
                pausedField.setAccessible(true);
                if (pausedField.getBoolean(record)) {
                    continue;
                }

                Field activityField = recordClass.getDeclaredField("activity");
                activityField.setAccessible(true);
                Activity activity = (Activity) activityField.get(record);
                if (activity != null) {
                    return activity;
                }
            }
        } catch (ReflectiveOperationException ignored) {
            return null;
        }
        return null;
    }
}
