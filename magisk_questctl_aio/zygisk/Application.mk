# zygisksu/NeoZygisk openat()s zygisk/armeabi-v7a.so even on arm64, so build both ABIs.
APP_ABI      := arm64-v8a armeabi-v7a
APP_PLATFORM := android-29
APP_STL      := c++_static
# -fexceptions/-frtti: FEATURE 5 crashproof catches libshell's std::length_error.
APP_CPPFLAGS := -std=c++17 -fexceptions -frtti
