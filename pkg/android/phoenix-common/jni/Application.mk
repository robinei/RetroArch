
APP_PLATFORM := android-9

APP_ABI := armeabi-v7a

NDK_TOOLCHAIN_VERSION := 4.9

APP_CFLAGS := \
    -march=armv7-a \
    -mfpu=neon \
    -mfloat-abi=softfp \
    -marm \
    -fprefetch-loop-arrays \
    -DHAVE_NEON=1

APP_CPPFLAGS := $(APP_CFLAGS)

APP_STL := gnustl_static

NDK_MODULE_PATH := /home/robin/Android/jniDeps
