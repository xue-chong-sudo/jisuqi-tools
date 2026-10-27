#!/usr/bin/env bash
# build_gitbash.sh —— 不依赖 vcvars64 的极速启构建（Git Bash 用）
#
# 为什么不用 build_local.bat：它 call vcvars64.bat，而 vcvars 内部会调 reg.exe
# 读注册表，本机安全策略把 reg.exe 拉黑了 → 环境变量没设上 → 找不到 windows.h。
# 这里把 build_local.bat 里的编译/链接参数原样搬过来，只把 INCLUDE/LIB 手动给全。
set -u
PROJ="C:/Users/Administrator/Desktop/极速启工具集/极速启"
MSVC="D:/Visual Studio 2022/VC/Tools/MSVC/14.51.36231"
SDK="D:/Windows Kits/10"
SDKV="10.0.26100.0"
export INCLUDE="$MSVC/include;$SDK/Include/$SDKV/um;$SDK/Include/$SDKV/shared;$SDK/Include/$SDKV/ucrt"
export LIB="$MSVC/lib/x64;$SDK/Lib/$SDKV/um/x64;$SDK/Lib/$SDKV/ucrt/x64"

cd "$PROJ" || exit 1

MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$MSVC/bin/Hostx64/x64/cl.exe" \
  /nologo /std:c++17 /O2 /MT /EHsc /Gy /utf-8 \
  /D_WIN32_WINNT=0x0A00 /D_WIN32_IE=0x0600 /DJISUQI_EMBEDDED /Iinclude /c \
  main.cpp App.cpp MainWindow.cpp TrayIcon.cpp MouseHook.cpp DragDrop.cpp \
  Config.cpp IconManager.cpp Pinyin.cpp AutoStart.cpp settings_ui.cpp jisuq_ui_layout.cpp || exit 1

MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$SDK/bin/$SDKV/x64/rc.exe" /nologo JisuQi.rc || exit 1

MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$MSVC/bin/Hostx64/x64/link.exe" \
  /nologo /SUBSYSTEM:WINDOWS /MACHINE:X64 /OUT:JisuQi.exe \
  main.obj App.obj MainWindow.obj TrayIcon.obj MouseHook.obj DragDrop.obj \
  Config.obj IconManager.obj Pinyin.obj AutoStart.obj settings_ui.obj jisuq_ui_layout.obj JisuQi.res \
  user32.lib gdiplus.lib shell32.lib ole32.lib comctl32.lib gdi32.lib uuid.lib imm32.lib dwmapi.lib shlwapi.lib
