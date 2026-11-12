package com.google.androidgamesdk;

import static android.app.NativeActivity.META_DATA_LIB_NAME;

import android.annotation.TargetApi;
import android.app.Activity;
import android.content.ComponentName;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.Window;
import android.view.WindowManager;
import java.lang.reflect.Method;
import java.util.Arrays;
import java.util.concurrent.locks.Condition;
import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantLock;

public class SwappyDisplayManager implements DisplayManager.DisplayListener {
    final private String LOG_TAG = "SwappyDisplayManager";
    final private boolean DEBUG = false;
    final private long ONE_MS_IN_NS = 1000000;
    final private long ONE_S_IN_NS = ONE_MS_IN_NS * 1000;
    private static final int NO_MODE_ID = -1;
    private static final int ANDROID_16_SDK_INT = 36;
    private static final Method sHasArrSupportMethod = resolveHasArrSupportMethod();
    private static final Method sSetRenderAheadEnabledMethod = resolveSetRenderAheadEnabledMethod();

    private long mCookie;
    private Activity mActivity;
    private DisplayManager mDisplayManager;
    private WindowManager mWindowManager;
    private Display.Mode mCurrentMode;
    private float mCurrentRefreshRate;
    private boolean mForwardRefreshCallbacks;

    private LooperThread mLooper;

    private static Method resolveHasArrSupportMethod() {
        if (Build.VERSION.SDK_INT < ANDROID_16_SDK_INT) {
            return null;
        }
        try {
            return Display.class.getMethod("hasArrSupport");
        } catch (NoSuchMethodException e) {
            return null;
        }
    }

    private static Method resolveSetRenderAheadEnabledMethod() {
        try {
            return Window.class.getMethod("setRenderAheadEnabled", boolean.class);
        } catch (NoSuchMethodException e) {
            return null;
        }
    }

    private static boolean hasArrSupport(Display display) {
        if (sHasArrSupportMethod == null) {
            return false;
        }
        try {
            Object result = sHasArrSupportMethod.invoke(display);
            return (result instanceof Boolean) && (Boolean) result;
        } catch (ReflectiveOperationException | RuntimeException e) {
            return false;
        }
    }

    // Mirrors native SwappyDisplayManager::useSwappyDisplayManager: on API 31+
    // (and API 30 S preview), NDKChoreographerThread owns refresh-rate callbacks.
    private static boolean shouldForwardRefreshCallbacks() {
        return !(Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
                || (Build.VERSION.SDK_INT == Build.VERSION_CODES.R
                        && Build.VERSION.PREVIEW_SDK_INT == 1));
    }

    private static boolean isValidRefreshRate(float refreshRate) {
        return refreshRate > 0.0f && !Float.isNaN(refreshRate) && !Float.isInfinite(refreshRate);
    }

    private long toPeriodNanos(float refreshRate) {
        return Math.round((double) ONE_S_IN_NS / (double) refreshRate);
    }

    private class LooperThread extends Thread {
        public Handler mHandler;
        private Lock mLock = new ReentrantLock();
        private Condition mCondition = mLock.newCondition();

        @Override
        public void start() {
            mLock.lock();
            super.start();
            try {
                mCondition.await();
            } catch (InterruptedException e) {
                e.printStackTrace();
            }
            mLock.unlock();
        }

        public void run() {
            Log.i(LOG_TAG, "Starting looper thread");

            mLock.lock();
            Looper.prepare();
            mHandler = new Handler();
            mCondition.signal();
            mLock.unlock();

            Looper.loop();

            Log.i(LOG_TAG, "Terminating looper thread");
        }
    }

    @TargetApi(Build.VERSION_CODES.M)
    private boolean modeMatchesCurrentResolution(Display.Mode mode) {
        return mode.getPhysicalHeight() == mCurrentMode.getPhysicalHeight()
                && mode.getPhysicalWidth() == mCurrentMode.getPhysicalWidth();
    }

    // Called from native SwappyDisplayManager.cpp
    public SwappyDisplayManager(long cookie, Activity activity) {
        // Load the native library for cases where an NDK application is running
        // without a java componenet
        try {
            ActivityInfo ai = activity.getPackageManager().getActivityInfo(
                    activity.getIntent().getComponent(), PackageManager.GET_META_DATA);
            if (ai.metaData != null) {
                String nativeLibName = ai.metaData.getString(META_DATA_LIB_NAME);
                if (nativeLibName != null) {
                    System.loadLibrary(nativeLibName);
                }
            }
        } catch (java.lang.Throwable e) {
            Log.e(LOG_TAG, e.getMessage());
        }

        mCookie = cookie;
        mActivity = activity;
        mForwardRefreshCallbacks = shouldForwardRefreshCallbacks();

        mDisplayManager = mActivity.getSystemService(DisplayManager.class);
        mWindowManager = mActivity.getSystemService(WindowManager.class);
        Display display = mWindowManager.getDefaultDisplay();
        mCurrentMode = display.getMode();
        mCurrentRefreshRate = display.getRefreshRate();
        updateSupportedRefreshRates(display);

        // Register display listener callbacks
        synchronized (this) {
            mLooper = new LooperThread();
            mLooper.start();
            mDisplayManager.registerDisplayListener(this, mLooper.mHandler);
        }
    }

    private static final class RefreshRateList {
        long[] mPeriods;
        int[] mModeIds;
        int mSize;

        RefreshRateList(int initialCapacity) {
            int capacity = Math.max(8, initialCapacity);
            mPeriods = new long[capacity];
            mModeIds = new int[capacity];
        }

        void add(long periodNanos, int modeId) {
            if (mSize == mPeriods.length) {
                mPeriods = Arrays.copyOf(mPeriods, mPeriods.length * 2);
                mModeIds = Arrays.copyOf(mModeIds, mModeIds.length * 2);
            }
            mPeriods[mSize] = periodNanos;
            mModeIds[mSize] = modeId;
            mSize++;
        }
    }

    private void updateSupportedRefreshRates(Display display) {
        Display.Mode[] supportedModes = display.getSupportedModes();
        if (supportedModes == null) {
            supportedModes = new Display.Mode[0];
        }

        RefreshRateList rates = new RefreshRateList(supportedModes.length * 2);
        for (int i = 0; i < supportedModes.length; i++) {
            Display.Mode mode = supportedModes[i];
            if (!modeMatchesCurrentResolution(mode)) {
                continue;
            }
            float modeRate = mode.getRefreshRate();
            if (isValidRefreshRate(modeRate)) {
                rates.add(toPeriodNanos(modeRate), mode.getModeId());
            }

            // API 31 (S) adds Mode.getAlternativeRefreshRates() for rates that
            // switch without a mode reset, covering devices where
            // Display.hasArrSupport() is false or unavailable.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                float[] altRates = mode.getAlternativeRefreshRates();
                if (altRates != null) {
                    for (int j = 0; j < altRates.length; j++) {
                        if (isValidRefreshRate(altRates[j])) {
                            rates.add(toPeriodNanos(altRates[j]), NO_MODE_ID);
                        }
                    }
                }
            }
        }

        if (hasArrSupport(display)) {
            float[] arrRates = display.getSupportedRefreshRates();
            if (arrRates != null) {
                for (int i = 0; i < arrRates.length; i++) {
                    if (isValidRefreshRate(arrRates[i])) {
                        rates.add(toPeriodNanos(arrRates[i]), NO_MODE_ID);
                    }
                }
            }
        }

        // Call down to native to set the supported refresh rates
        nSetSupportedRefreshPeriods(mCookie, Arrays.copyOf(rates.mPeriods, rates.mSize),
                Arrays.copyOf(rates.mModeIds, rates.mSize));
    }

    // Called from native SwappyDisplayManager.cpp
    public void setPreferredDisplayModeId(final int modeId) {
        mActivity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Window w = mActivity.getWindow();
                WindowManager.LayoutParams l = w.getAttributes();
                if (DEBUG) {
                    Log.v(LOG_TAG, "set preferredDisplayModeId to " + modeId);
                }
                l.preferredDisplayModeId = modeId;

                w.setAttributes(l);
            }
        });
    }

    // Called from native SwappyDisplayManager.cpp
    public void disableRenderAhead() {
        if (sSetRenderAheadEnabledMethod == null) {
            return;
        }
        final Activity activity = mActivity;
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                Window w = activity.getWindow();
                if (w == null) {
                    return;
                }
                try {
                    sSetRenderAheadEnabledMethod.invoke(w, false);
                } catch (ReflectiveOperationException | RuntimeException e) {
                    Log.w(LOG_TAG, "Failed to disable render-ahead", e);
                }
            }
        });
    }

    // Called from native SwappyDisplayManager.cpp
    public synchronized void enableRefreshCallbackForwarding() {
        mForwardRefreshCallbacks = true;
    }

    // Called from native SwappyDisplayManager.cpp
    public void terminate() {
        mDisplayManager.unregisterDisplayListener(this);
        mLooper.mHandler.getLooper().quit();
    }

    @Override
    public void onDisplayAdded(int displayId) {}

    @Override
    public void onDisplayRemoved(int displayId) {}

    @Override
    public void onDisplayChanged(int displayId) {
        synchronized (this) {
            Display display = mWindowManager.getDefaultDisplay();
            float newRefreshRate = display.getRefreshRate();
            Display.Mode newMode = display.getMode();
            boolean modeChanged = newMode.getModeId() != mCurrentMode.getModeId();
            boolean resolutionChanged =
                    (newMode.getPhysicalWidth() != mCurrentMode.getPhysicalWidth())
                    || (newMode.getPhysicalHeight() != mCurrentMode.getPhysicalHeight());
            boolean refreshRateChanged = (newRefreshRate != mCurrentRefreshRate);
            mCurrentMode = newMode;
            mCurrentRefreshRate = newRefreshRate;

            if (resolutionChanged || (modeChanged && hasArrSupport(display))) {
                updateSupportedRefreshRates(display);
            }

            if (mForwardRefreshCallbacks && refreshRateChanged
                    && isValidRefreshRate(newRefreshRate)) {
                final long appVsyncOffsetNanos = display.getAppVsyncOffsetNanos();
                final long vsyncPresentationDeadlineNanos = display.getPresentationDeadlineNanos();

                final long vsyncPeriodNanos = toPeriodNanos(newRefreshRate);
                final long sfVsyncOffsetNanos =
                        vsyncPeriodNanos - (vsyncPresentationDeadlineNanos - ONE_MS_IN_NS);

                nOnRefreshPeriodChanged(
                        mCookie, vsyncPeriodNanos, appVsyncOffsetNanos, sfVsyncOffsetNanos);
            }
        }
    }

    private native void nSetSupportedRefreshPeriods(
            long cookie, long[] refreshPeriods, int[] modeIds);
    private native void nOnRefreshPeriodChanged(
            long cookie, long refreshPeriod, long appOffset, long sfOffset);
}
