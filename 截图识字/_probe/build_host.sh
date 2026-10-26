#!/usr/bin/env bash
# 全量重建 dist/screenshot_host.exe（等价 build_host.bat，但走 Git Bash + 绝对路径）
set -u
PROJ="C:/Users/Administrator/Desktop/极速启工具集/截图识字"
MSVC="D:/Visual Studio 2022/VC/Tools/MSVC/14.51.36231"
SDK="D:/Windows Kits/10"
SDKV="10.0.26100.0"
export INCLUDE="$MSVC/include;$SDK/Include/$SDKV/um;$SDK/Include/$SDKV/shared;$SDK/Include/$SDKV/ucrt;$PROJ/deps/tesseract/include"
export LIB="$MSVC/lib/x64;$SDK/Lib/$SDKV/um/x64;$SDK/Lib/$SDKV/ucrt/x64;$PROJ/deps/tesseract/lib"

cd "$PROJ/res" || exit 1
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$SDK/bin/$SDKV/x64/rc.exe" \
  /nologo /fo "$PROJ/res/app.res" app.rc || exit 1

cd "$PROJ" || exit 1
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$MSVC/bin/Hostx64/x64/cl.exe" \
  /nologo /O2 /EHsc /utf-8 /MD /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS \
  "/I$PROJ/src" "/I$PROJ/deps/tesseract/include" \
  src/main.cpp src/capture.cpp src/view.cpp src/ocr_tesseract.cpp src/ocraddon.cpp "$PROJ/res/app.res" \
  /Fe:"$PROJ/dist/screenshot_host.exe" \
  /link user32.lib gdi32.lib gdiplus.lib dwmapi.lib ws2_32.lib shell32.lib advapi32.lib shlwapi.lib comdlg32.lib \
  ucrt.lib vcruntime.lib msvcrt.lib oldnames.lib \
  "$PROJ/deps/tesseract/lib_static/tesseract55.lib" "$PROJ/deps/tesseract/lib/leptonica-1.87.0.lib" \
  /SUBSYSTEM:WINDOWS /MACHINE:X64
