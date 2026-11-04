#include <jni.h>
#include <stdlib.h>
#include <unistd.h>

#include <atomic>

#include "game-activity/native_app_glue/android_native_app_glue.h"

#ifdef AGDK_COVERAGE
#include "gcov_flush.h"
#endif

namespace {
int on_pause_sleep_duration_ms = 0;
std::atomic<int> last_trim_memory_level{-1};
std::atomic<int> last_key_code{-1};
std::atomic<bool> reject_back_key{false};

bool key_event_filter(const GameActivityKeyEvent* event) {
    return !(reject_back_key && event->keyCode == AKEYCODE_BACK);
}

void record_key_events(struct android_app* app) {
    android_input_buffer* input_buffer = android_app_swap_input_buffers(app);
    if (input_buffer == nullptr) return;
    for (uint64_t i = 0; i < input_buffer->keyEventsCount; ++i) {
        last_key_code = input_buffer->keyEvents[i].keyCode;
    }
    android_app_clear_key_events(input_buffer);
    android_app_clear_motion_events(input_buffer);
}
} // namespace

void handle_cmd(struct android_app* app, int32_t cmd) {
    if (cmd == APP_CMD_PAUSE) {
        if (on_pause_sleep_duration_ms > 0) {
            usleep(on_pause_sleep_duration_ms * 1000);
        }
    }
    if (cmd == APP_CMD_LOW_MEMORY) {
        last_trim_memory_level = app->trimMemoryLevel;
    }
#ifdef AGDK_COVERAGE
    if (cmd == APP_CMD_PAUSE || cmd == APP_CMD_STOP || cmd == APP_CMD_DESTROY) {
        agdk::testing::FlushGcovCounters();
    }
#endif
}

extern "C" {

JNIEXPORT void JNICALL Java_com_google_androidgamesdk_TestGameActivity_setOnPauseSleepDurationMs(
        JNIEnv* env, jclass clazz, jint durationMs) {
    on_pause_sleep_duration_ms = durationMs;
}

JNIEXPORT jint JNICALL
Java_com_google_androidgamesdk_TestGameActivity_getLastTrimMemoryLevel(JNIEnv* env, jclass clazz) {
    return last_trim_memory_level;
}

JNIEXPORT jintArray JNICALL
Java_com_google_androidgamesdk_TestGameActivity_getTrimMemoryLevels(JNIEnv* env, jclass clazz) {
    const jint levels[] = {
            GAMEACTIVITY_TRIM_MEMORY_RUNNING_MODERATE, GAMEACTIVITY_TRIM_MEMORY_RUNNING_LOW,
            GAMEACTIVITY_TRIM_MEMORY_RUNNING_CRITICAL, GAMEACTIVITY_TRIM_MEMORY_UI_HIDDEN,
            GAMEACTIVITY_TRIM_MEMORY_BACKGROUND,       GAMEACTIVITY_TRIM_MEMORY_MODERATE,
            GAMEACTIVITY_TRIM_MEMORY_COMPLETE,
    };
    const jsize count = sizeof(levels) / sizeof(levels[0]);
    jintArray result = env->NewIntArray(count);
    env->SetIntArrayRegion(result, 0, count, levels);
    return result;
}

JNIEXPORT jint JNICALL
Java_com_google_androidgamesdk_TestGameActivity_getLastKeyCode(JNIEnv* env, jclass clazz) {
    return last_key_code;
}

JNIEXPORT void JNICALL Java_com_google_androidgamesdk_TestGameActivity_setRejectBackKey(
        JNIEnv* env, jclass clazz, jboolean reject) {
    reject_back_key = reject;
}

JNIEXPORT jfloat JNICALL
Java_com_google_androidgamesdk_TestGameActivity_getPinchScaleFactorAxisValue(JNIEnv* env,
                                                                             jclass clazz,
                                                                             jfloat value) {
    GameActivityPointerAxes pointer = {};
    pointer.axisValues[AMOTION_EVENT_AXIS_GESTURE_PINCH_SCALE_FACTOR] = value;
    GameActivityPointerAxes_enableAxis(AMOTION_EVENT_AXIS_GESTURE_PINCH_SCALE_FACTOR);
    float result =
            GameActivityPointerAxes_getAxisValue(&pointer,
                                                 AMOTION_EVENT_AXIS_GESTURE_PINCH_SCALE_FACTOR);
    GameActivityPointerAxes_disableAxis(AMOTION_EVENT_AXIS_GESTURE_PINCH_SCALE_FACTOR);
    return result;
}

void android_main(struct android_app* app) {
    app->onAppCmd = handle_cmd;
    android_app_set_key_event_filter(app, key_event_filter);

    while (true) {
        int events;
        struct android_poll_source* source;

        while (ALooper_pollOnce(0, nullptr, &events, (void**)&source) >= 0) {
            if (source) source->process(app, source);
            record_key_events(app);
            if (app->destroyRequested) {
#ifdef AGDK_COVERAGE
                agdk::testing::FlushGcovCounters();
#endif
                return;
            }
        }
    }
}
}