/* language: C++17, file: android/hwcomposer/xd_log.h
 * Thin logging: liblog on NDK, stderr on host syntax checks.
 */
#ifndef XD_LOG_H
#define XD_LOG_H

#ifdef __ANDROID__
#include <android/log.h>
#define XD_TAG "x11droid-hwc"
#define XD_LOGI(...) __android_log_print(ANDROID_LOG_INFO, XD_TAG, __VA_ARGS__)
#define XD_LOGW(...) __android_log_print(ANDROID_LOG_WARN, XD_TAG, __VA_ARGS__)
#define XD_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, XD_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define XD_LOGI(...) (std::fprintf(stderr, "[hwc I] " __VA_ARGS__), std::fputc('\n', stderr))
#define XD_LOGW(...) (std::fprintf(stderr, "[hwc W] " __VA_ARGS__), std::fputc('\n', stderr))
#define XD_LOGE(...) (std::fprintf(stderr, "[hwc E] " __VA_ARGS__), std::fputc('\n', stderr))
#endif

#endif /* XD_LOG_H */
