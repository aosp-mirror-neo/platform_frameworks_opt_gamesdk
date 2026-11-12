/*
 * Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.swappy.testapp;

import android.app.Activity;
import android.app.Application;
import android.app.Presentation;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.PixelFormat;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.ImageReader;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.view.Display;
import java.lang.reflect.Method;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Owns the real Android Contexts (and any launched Activity or VirtualDisplay) requested by native
 * tests over JNI, releasing them when {@link #close()} is called.
 */
public final class TestContexts {
    private static final long ACTIVITY_LAUNCH_TIMEOUT_SECONDS = 10;

    private static volatile ActivityHandoff sPendingLaunch;

    private Activity mActivity;
    private ImageReader mImageReader;
    private VirtualDisplay mVirtualDisplay;

    public TestContexts() {}

    private static final class ActivityHandoff {
        final CountDownLatch createdLatch = new CountDownLatch(1);
        volatile Activity activity;

        void onCreated(Activity createdActivity) {
            activity = createdActivity;
            createdLatch.countDown();
        }
    }

    /** Minimal Activity used to test Activity-hosted Swappy without running MainActivity. */
    public static final class HostActivity extends Activity {
        @Override
        protected void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);
            ActivityHandoff handoff = sPendingLaunch;
            if (handoff != null) {
                handoff.onCreated(this);
            } else {
                finish();
            }
        }
    }

    /** A Service that was never started by the system, but is backed by the real application. */
    private static class TestService extends Service {
        TestService(Context applicationContext) {
            attachBaseContext(applicationContext);
        }

        @Override
        public IBinder onBind(Intent intent) {
            return null;
        }
    }

    public Application getApplication() throws ReflectiveOperationException {
        Method currentApplication =
                Class.forName("android.app.ActivityThread").getMethod("currentApplication");
        return (Application) currentApplication.invoke(null);
    }

    public Service createService() throws ReflectiveOperationException {
        return new TestService(getApplication());
    }

    public Context createDisplayContext() throws ReflectiveOperationException {
        Display display = ensureVirtualDisplay();
        if (display == null) {
            return null;
        }
        return getApplication().createDisplayContext(display);
    }

    public Context createPresentationContext()
            throws ReflectiveOperationException, InterruptedException {
        Display display = ensureVirtualDisplay();
        if (display == null) {
            return null;
        }
        Application application = getApplication();
        // Presentation extends Dialog, whose constructor creates a Handler on the caller's Looper.
        Context[] presentationContextHolder = new Context[1];
        Throwable[] errorHolder = new Throwable[1];
        CountDownLatch presentationCreated = new CountDownLatch(1);
        new Handler(Looper.getMainLooper()).post(() -> {
            try {
                presentationContextHolder[0] = new Presentation(application, display).getContext();
            } catch (Throwable t) {
                errorHolder[0] = t;
            } finally {
                presentationCreated.countDown();
            }
        });
        if (!presentationCreated.await(ACTIVITY_LAUNCH_TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
            return null;
        }
        if (errorHolder[0] != null) {
            throw new RuntimeException("Failed to create Presentation", errorHolder[0]);
        }
        return presentationContextHolder[0];
    }

    public int getVirtualDisplayId() {
        if (mVirtualDisplay == null || mVirtualDisplay.getDisplay() == null) {
            return 0;
        }
        return mVirtualDisplay.getDisplay().getDisplayId();
    }

    /** Starts a HostActivity and waits until it is created. */
    public Activity launchActivity() throws ReflectiveOperationException, InterruptedException {
        if (mActivity != null) {
            return mActivity;
        }
        ActivityHandoff handoff = new ActivityHandoff();
        sPendingLaunch = handoff;
        try {
            Application application = getApplication();
            Intent intent = new Intent(application, HostActivity.class);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            application.startActivity(intent);
            if (!handoff.createdLatch.await(ACTIVITY_LAUNCH_TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
                return null;
            }
            mActivity = handoff.activity;
            return mActivity;
        } finally {
            sPendingLaunch = null;
            if (mActivity == null && handoff.activity != null) {
                handoff.activity.finish();
            }
        }
    }

    public void close() {
        if (mActivity != null) {
            mActivity.finish();
            mActivity = null;
        }
        if (mVirtualDisplay != null) {
            mVirtualDisplay.release();
            mVirtualDisplay = null;
        }
        if (mImageReader != null) {
            mImageReader.close();
            mImageReader = null;
        }
    }

    private Display ensureVirtualDisplay() throws ReflectiveOperationException {
        if (mVirtualDisplay != null) {
            return mVirtualDisplay.getDisplay();
        }
        Application application = getApplication();
        DisplayManager displayManager = application.getSystemService(DisplayManager.class);
        ImageReader imageReader = ImageReader.newInstance(640, 480, PixelFormat.RGBA_8888, 2);
        VirtualDisplay virtualDisplay = null;
        try {
            virtualDisplay = displayManager.createVirtualDisplay("SwappyTestVirtualDisplay", 640,
                    480, DisplayMetrics.DENSITY_MEDIUM, imageReader.getSurface(), 0);
            if (virtualDisplay == null || virtualDisplay.getDisplay() == null) {
                return null;
            }
            mImageReader = imageReader;
            mVirtualDisplay = virtualDisplay;
            return virtualDisplay.getDisplay();
        } finally {
            if (mVirtualDisplay == null) {
                if (virtualDisplay != null) {
                    virtualDisplay.release();
                }
                imageReader.close();
            }
        }
    }
}
