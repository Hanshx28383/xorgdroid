# language: Makefile, file: android/gles_stream/Android.mk
# Build libGLES_x11droid.so (Android NDK)
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE    := GLES_x11droid
LOCAL_SRC_FILES := src/gles_client.c
LOCAL_C_INCLUDES := $(LOCAL_PATH)/include
LOCAL_CFLAGS    := -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Wshadow -Wformat=2
LOCAL_LDLIBS    := -llog -ldl
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/include

include $(BUILD_SHARED_LIBRARY)