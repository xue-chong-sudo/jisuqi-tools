#!/usr/bin/env bash
# 编译 ocr_time_probe：需要真正链上 tesseract 静态库（不是桩）
set -u
PROJ="C:/Users/Administrator/Desktop/极速启工具集/截图识字"
MSVC="D:/Visual Studio 2022/VC/Tools/MSVC/14.51.36231"
SDK="D:/Windows Kits/10"
SDKV="10.0.26100.0"
export INCLUDE="$MSVC/include;$SDK/Include/$SDKV/um;$SDK/Include/$SDKV/shared;$SDK/Include/$SDKV/ucrt;$PROJ/deps/tesseract/include;$PROJ/src"
export LIB="$MSVC/lib/x64;$SDK/Lib/$SDKV/um/x64;$SDK/Lib/$SDKV/ucrt/x64;$PROJ/deps/tesseract/lib"

cd "$PROJ/_probe" || exit 1
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$MSVC/bin/Hostx64/x64/cl.exe" \
  /nologo /O2 /EHsc /utf-8 /MD /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS \
  ocr_time_probe.cpp "$PROJ/src/ocr_tesseract.cpp" "$PROJ/src/ocraddon.cpp" \
  /Fe:ocr_time_probe.exe \
  /link user32.lib gdi32.lib \
  "$PROJ/deps/tesseract/lib_static/tesseract55.lib" "$PROJ/deps/tesseract/lib/leptonica-1.87.0.lib" \
  /SUBSYSTEM:CONSOLE /MACHINE:X64
