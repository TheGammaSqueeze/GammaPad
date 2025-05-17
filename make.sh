ARCH=arm64 \
aarch64-linux-android33-clang \
-O3 \
gammapad_main.c \
gammapad_controller.c \
gammapad_inputdefs.c \
gammapad_ff.c \
gammapad_commands.c \
gammapad_capture.c \
gammapad_config.c \
-o gammapad


aarch64-linux-android33-clang \
-O3 \
rumbletest.c \
-o rumbletest
