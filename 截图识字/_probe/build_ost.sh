#!/usr/bin/env bash
# 编译 overlay_state_test.exe（直接 #include ../src/capture.cpp 的回归测试）
set -u
PROJ="C:/Users/Administrator/Desktop/极速启工具集/截图识字"
MSVC="D:/Visual Studio 2022/VC/Tools/MSVC/14.51.36231"
SDK="D:/Windows Kits/10"
SDKV="10.0.26100.0"
export INCLUDE="$MSVC/include;$SDK/Include/$SDKV/um;$SDK/Include/$SDKV/shared;$SDK/Include/$SDKV/ucrt;$PROJ/deps/tesseract/include"
export LIB="$MSVC/lib/x64;$SDK/Lib/$SDKV/um/x64;$SDK/Lib/$SDKV/ucrt/x64"

cd "$PROJ/_probe" || exit 1
TARGET="${1:-overlay_state_test.cpp}"
OUT="${2:-overlay_state_test.exe}"
EXTRA="${3:-}"
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$MSVC/bin/Hostx64/x64/cl.exe" \
  /nologo /O2 /EHsc /utf-8 /MD /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS $EXTRA \
  "$TARGET" /Fe:"$OUT" \
  /link user32.lib gdi32.lib dwmapi.lib gdiplus.lib comdlg32.lib shell32.lib /SUBSYSTEM:CONSOLE /MACHINE:X64
