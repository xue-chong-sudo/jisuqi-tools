#!/usr/bin/env bash
# 编译 cap_drv.exe（控制台程序，只需 user32）。
# 注意：必须 MSYS_NO_PATHCONV=1，否则 Git Bash 会把 /nologo 当成 "D:/.../nologo" 路径，
#       于是 /utf-8 也丢了 —— UTF-8 注释里的中文按 GBK 解码后行尾出现 0x5C(\)，
#       把下一行 #include <windows.h> 拼进注释，导致 HWND 未定义的一大串假报错。
set -u
PROJ="C:/Users/Administrator/Desktop/极速启工具集/截图识字"
MSVC="D:/Visual Studio 2022/VC/Tools/MSVC/14.51.36231"
SDK="D:/Windows Kits/10"
SDKV="10.0.26100.0"
CL="$MSVC/bin/Hostx64/x64/cl.exe"
export INCLUDE="$MSVC/include;$SDK/Include/$SDKV/um;$SDK/Include/$SDKV/shared;$SDK/Include/$SDKV/ucrt"
export LIB="$MSVC/lib/x64;$SDK/Lib/$SDKV/um/x64;$SDK/Lib/$SDKV/ucrt/x64"

cd "$PROJ/_probe" || exit 1
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$CL" \
  /nologo /O2 /EHsc /utf-8 /MD /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS \
  cap_drv3.cpp /Fe:cap_drv3.exe \
  /link user32.lib gdi32.lib /SUBSYSTEM:CONSOLE /MACHINE:X64
