ARCH=arm64 \
/root/android-ndk-r25c/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android33-clang \
-O3 \
gammapad_main.c \
gammapad_controller.c \
gammapad_inputdefs.c \
gammapad_ff.c \
gammapad_commands.c \
gammapad_capture.c \
-o gammapad


/root/android-ndk-r25c/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android33-clang \
-O3 \
rumbletest.c \
-o rumbletest
