#!/bin/bash
# Compile GammaPad for Android ARM64 (aarch64)

AOSP_ROOT="/home/david/develop/GammaOSNextDistribution-14"
CLANG="$AOSP_ROOT/prebuilts/clang/host/linux-x86/clang-r498229b/bin/clang"
CLANG_INCLUDE="$AOSP_ROOT/prebuilts/clang/host/linux-x86/clang-r498229b/lib/clang/17/include"

# Bionic headers and kernel headers
BIONIC_INCLUDE="$AOSP_ROOT/bionic/libc/include"
BIONIC_KERNEL="$AOSP_ROOT/bionic/libc/kernel/uapi"
BIONIC_KERNEL_ASM="$AOSP_ROOT/bionic/libc/kernel/uapi/asm-arm64"
BIONIC_ARCH="$AOSP_ROOT/bionic/libc/kernel/android/uapi"

# Runtime libs for arm64
RUNTIME_LIB="$AOSP_ROOT/prebuilts/runtime/mainline/runtime/sdk/android/arm64/lib"

echo "Building gammapad..."

$CLANG --target=aarch64-linux-android33 \
    -O3 \
    -nostdinc \
    -nostdlib \
    -isystem "$CLANG_INCLUDE" \
    -isystem "$BIONIC_INCLUDE" \
    -isystem "$BIONIC_KERNEL" \
    -isystem "$BIONIC_KERNEL_ASM" \
    -isystem "$BIONIC_ARCH" \
    -D__ANDROID__ \
    -fPIE \
    -pie \
    -Wl,-dynamic-linker,/system/bin/linker64 \
    -L"$RUNTIME_LIB" \
    "$RUNTIME_LIB/crtbegin_dynamic.o" \
    gammapad_main.c \
    gammapad_controller.c \
    gammapad_inputdefs.c \
    gammapad_ff.c \
    gammapad_commands.c \
    gammapad_capture.c \
    gammapad_config.c \
    gammapad_calibration.c \
    "$RUNTIME_LIB/libc.so" \
    "$RUNTIME_LIB/libdl.so" \
    "$RUNTIME_LIB/crtend_android.o" \
    -o gammapad 2>&1

if [ -f gammapad ]; then
    echo "Built successfully: gammapad"
    ls -la gammapad
    file gammapad
else
    echo "Build failed."
fi

echo ""
echo "Building rumbletest..."

$CLANG --target=aarch64-linux-android33 \
    -O3 \
    -nostdinc \
    -nostdlib \
    -isystem "$CLANG_INCLUDE" \
    -isystem "$BIONIC_INCLUDE" \
    -isystem "$BIONIC_KERNEL" \
    -isystem "$BIONIC_KERNEL_ASM" \
    -isystem "$BIONIC_ARCH" \
    -D__ANDROID__ \
    -fPIE \
    -pie \
    -Wl,-dynamic-linker,/system/bin/linker64 \
    -L"$RUNTIME_LIB" \
    "$RUNTIME_LIB/crtbegin_dynamic.o" \
    rumbletest.c \
    "$RUNTIME_LIB/libc.so" \
    "$RUNTIME_LIB/libdl.so" \
    "$RUNTIME_LIB/crtend_android.o" \
    -o rumbletest 2>&1

if [ -f rumbletest ]; then
    echo "Built successfully: rumbletest"
    ls -la rumbletest
fi
