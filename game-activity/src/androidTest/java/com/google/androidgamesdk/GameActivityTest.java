/*
 * Copyright (C) 2026 The Android Open Source Project
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

package com.google.androidgamesdk;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertTrue;

import android.app.Activity;
import android.content.ComponentCallbacks2;
import android.os.SystemClock;
import android.util.Log;
import android.view.KeyEvent;
import androidx.lifecycle.Lifecycle;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.rules.ActivityScenarioRule;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class GameActivityTest {
    private static final String TAG = "GameActivityTest";

    @Rule
    public ActivityScenarioRule<TestGameActivity> rule =
            new ActivityScenarioRule<>(TestGameActivity.class);

    @Test
    public void onPause_whenNativeThreadIsBusy_logsWarning()
            throws IOException, InterruptedException {
        // clear logcat
        Runtime.getRuntime().exec("logcat -c").waitFor();

        ActivityScenario<TestGameActivity> scenario = rule.getScenario();

        // 1.5 seconds will trigger a warning (> 1s)
        TestGameActivity.setOnPauseSleepDurationMs(1500);

        scenario.recreate();

        Process process = Runtime.getRuntime().exec("logcat -d");
        BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream()));
        StringBuilder logcat = new StringBuilder();
        String line;
        while ((line = reader.readLine()) != null) {
            logcat.append(line).append("\n");
        }

        String logcatString = logcat.toString();
        assertTrue("Expected warning log not found",
                logcatString.contains("Application callback for event APP_CMD_PAUSE took"));
        assertTrue("Expected warning log signature missing",
                logcatString.contains(" W threaded_app: Application callback"));
    }

    @Test
    public void onPause_whenNativeThreadIsBusy_logsError()
            throws IOException, InterruptedException {
        // clear logcat
        Runtime.getRuntime().exec("logcat -c").waitFor();

        ActivityScenario<TestGameActivity> scenario = rule.getScenario();

        // 3.5 seconds will trigger an error (> 3s)
        TestGameActivity.setOnPauseSleepDurationMs(3500);

        scenario.recreate();

        Process process = Runtime.getRuntime().exec("logcat -d");
        BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream()));
        StringBuilder logcat = new StringBuilder();
        String line;
        while ((line = reader.readLine()) != null) {
            logcat.append(line).append("\n");
        }

        String logcatString = logcat.toString();
        assertTrue("Expected error log not found",
                logcatString.contains("Application callback for event APP_CMD_PAUSE took"));
        assertTrue("Expected error log signature missing",
                logcatString.contains(" E threaded_app: Application callback"));
    }

    @Test
    public void onTrimMemory_passesLevelToLowMemoryCommand() {
        ActivityScenario<TestGameActivity> scenario = rule.getScenario();

        scenario.onActivity(
                activity -> activity.onTrimMemory(ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN));
        assertEquals(ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN,
                waitForTrimMemoryLevel(ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN));

        scenario.onActivity(
                activity -> activity.onTrimMemory(ComponentCallbacks2.TRIM_MEMORY_BACKGROUND));
        assertEquals(ComponentCallbacks2.TRIM_MEMORY_BACKGROUND,
                waitForTrimMemoryLevel(ComponentCallbacks2.TRIM_MEMORY_BACKGROUND));
    }

    @Test
    public void trimMemoryLevels_matchComponentCallbacks2() {
        int[] expected = {
                ComponentCallbacks2.TRIM_MEMORY_RUNNING_MODERATE,
                ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW,
                ComponentCallbacks2.TRIM_MEMORY_RUNNING_CRITICAL,
                ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN,
                ComponentCallbacks2.TRIM_MEMORY_BACKGROUND,
                ComponentCallbacks2.TRIM_MEMORY_MODERATE,
                ComponentCallbacks2.TRIM_MEMORY_COMPLETE,
        };
        assertArrayEquals(expected, TestGameActivity.getTrimMemoryLevels());
    }

    @Test
    public void back_sendsBackKeyToNative() {
        ActivityScenario<TestGameActivity> scenario = rule.getScenario();

        // On API 33+ the test app enables predictive back, so this goes to OnBackInvokedCallback.
        InstrumentationRegistry.getInstrumentation().sendKeyDownUpSync(KeyEvent.KEYCODE_BACK);

        assertEquals(KeyEvent.KEYCODE_BACK, waitForLastKeyCode(KeyEvent.KEYCODE_BACK));
        scenario.onActivity(activity -> assertFalse(activity.isFinishing()));
    }

    @Test
    public void back_whenNativeRejectsBackKey_leavesActivity() {
        ActivityScenario<TestGameActivity> scenario = rule.getScenario();
        TestGameActivity.setRejectBackKey(true);
        try {
            InstrumentationRegistry.getInstrumentation().sendKeyDownUpSync(KeyEvent.KEYCODE_BACK);
            // The default back behavior finishes the activity or, for a task root on API 31+,
            // moves the task to the back.
            long deadline = SystemClock.uptimeMillis() + 5000;
            while (scenario.getState() == Lifecycle.State.RESUMED
                    && SystemClock.uptimeMillis() < deadline) {
                SystemClock.sleep(10);
            }
            assertNotEquals(Lifecycle.State.RESUMED, scenario.getState());
        } finally {
            TestGameActivity.setRejectBackKey(false);
        }
    }

    @Test
    public void gestureAxis_canBeEnabledAndRead() {
        assertEquals(2.5f, TestGameActivity.getPinchScaleFactorAxisValue(2.5f), 0f);
    }

    /** Waits up to 5 seconds for the native thread to see a key event with keyCode. */
    private static int waitForLastKeyCode(int keyCode) {
        long deadline = SystemClock.uptimeMillis() + 5000;
        int lastKeyCode = TestGameActivity.getLastKeyCode();
        while (lastKeyCode != keyCode && SystemClock.uptimeMillis() < deadline) {
            SystemClock.sleep(10);
            lastKeyCode = TestGameActivity.getLastKeyCode();
        }
        return lastKeyCode;
    }

    /** Waits up to 5 seconds for the native thread to handle APP_CMD_LOW_MEMORY. */
    private static int waitForTrimMemoryLevel(int expectedLevel) {
        long deadline = SystemClock.uptimeMillis() + 5000;
        int level = TestGameActivity.getLastTrimMemoryLevel();
        while (level != expectedLevel && SystemClock.uptimeMillis() < deadline) {
            SystemClock.sleep(10);
            level = TestGameActivity.getLastTrimMemoryLevel();
        }
        return level;
    }
}
