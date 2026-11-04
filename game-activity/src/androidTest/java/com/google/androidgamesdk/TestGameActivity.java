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

import android.os.Bundle;

public class TestGameActivity extends GameActivity {
    static {
        System.loadLibrary("gameactivity_test_native");
    }

    public static native void setOnPauseSleepDurationMs(int durationMs);

    /** Returns the trim level seen by the last APP_CMD_LOW_MEMORY handler, or -1. */
    public static native int getLastTrimMemoryLevel();

    /** Returns the GameActivityTrimMemoryLevel values, from RUNNING_MODERATE to COMPLETE. */
    public static native int[] getTrimMemoryLevels();

    /** Returns the key code of the last key event seen by the native thread, or -1. */
    public static native int getLastKeyCode();

    /** Makes the native key event filter reject AKEYCODE_BACK when reject is true. */
    public static native void setRejectBackKey(boolean reject);

    /**
     * Enables AMOTION_EVENT_AXIS_GESTURE_PINCH_SCALE_FACTOR, reads it from a pointer that holds
     * value on that axis, disables the axis and returns the value read.
     */
    public static native float getPinchScaleFactorAxisValue(float value);
}
