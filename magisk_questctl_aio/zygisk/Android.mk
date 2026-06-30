LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE     := questctl_aio
LOCAL_SRC_FILES  := module.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)
# -fexceptions + -frtti are REQUIRED for FEATURE 5 (crashproof): the C++ wrapper
# catches a std::length_error thrown by libshell across ORIG's frame (libshell has
# intact unwind tables); base-class catch (std::exception) needs RTTI type matching.
LOCAL_CPPFLAGS   := -std=c++17 -fvisibility=hidden -fexceptions -frtti -Os
LOCAL_LDLIBS     := -llog
include $(BUILD_SHARED_LIBRARY)
