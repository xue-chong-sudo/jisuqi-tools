#!/usr/bin/env bash
# 编译 _probe 下的一个驱动。用法：build_drv.sh cap_drv4 [输出名]
# 必须 MSYS_NO_PATHCONV=1，否则 /utf-8 丢失 → UTF-8 中文注释按 GBK 解码后行尾 0x5C
# 把下一行 #include 拼进注释，报一堆"HWND 未定义"的假错误。
set -u
PROJ="C:/Users/Administrator/Desktop/极速启工具集/截图识字"
MSVC="D:/Visual Studio 2022/VC/Tools/MSVC/14.51.36231"
SDK="D:/Windows Kits/10"
SDKV="10.0.26100.0"
export INCLUDE="$MSVC/include;$SDK/Include/$SDKV/um;$SDK/Include/$SDKV/shared;$SDK/Include/$SDKV/ucrt"
export LIB="$MSVC/lib/x64;$SDK/Lib/$SDKV/um/x64;$SDK/Lib/$SDKV/ucrt/x64"

SRC="${1:-cap_drv4}"
OUT="${2:-$SRC.exe}"
cd "$PROJ/_probe" || exit 1
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$MSVC/bin/Hostx64/x64/cl.exe" \
  /nologo /O2 /EHsc /utf-8 /MD /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS \
  "$SRC.cpp" /Fe:"$OUT" \
  /link user32.lib gdi32.lib psapi.lib shell32.lib /SUBSYSTEM:CONSOLE /MACHINE:X64
