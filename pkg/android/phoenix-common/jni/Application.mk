ifeq ($(GLES),3)
   ifndef NDK_GL_HEADER_VER
      APP_PLATFORM := android-18
   else
      APP_PLATFORM := $(NDK_GL_HEADER_VER)
   endif
else
   ifndef NDK_NO_GL_HEADER_VER
      APP_PLATFORM := android-9
   else
      APP_PLATFORM := $(NDK_NO_GL_HEADER_VER)
   endif
endif

ifndef TARGET_ABIS
   APP_ABI := armeabi-v7a
else
   APP_ABI := $(TARGET_ABIS)
endif

ifeq ($(USE_CLANG),1)
   NDK_TOOLCHAIN_VERSION := clang
   APP_CFLAGS   := -Wno-invalid-source-encoding
   APP_CPPFLAGS := -Wno-invalid-source-encoding
endif

APP_STL := c++_static
