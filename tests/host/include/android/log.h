#pragma once

// Host tests exercise the real socket implementation; only Android logging is stubbed.
enum {
    ANDROID_LOG_VERBOSE = 2,
    ANDROID_LOG_DEBUG,
    ANDROID_LOG_INFO,
    ANDROID_LOG_WARN,
    ANDROID_LOG_ERROR,
    ANDROID_LOG_FATAL,
};

inline int __android_log_print(int, const char *, const char *, ...) { return 0; }
