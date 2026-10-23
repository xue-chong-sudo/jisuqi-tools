@echo off
setlocal
cd /d "%~dp0"

REM Load MSVC x64 env (also sets Windows SDK INCLUDE/LIB/PATH)
call "D:\Visual Studio 2022\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [ERROR] vcvars64.bat failed & exit /b 1 )

echo [INFO] Toolchain check:
where cl.exe
where rc.exe
where link.exe

echo [INFO] Compiling...
cl.exe /nologo /std:c++17 /O2 /MT /EHsc /Gy /utf-8 /D_WIN32_WINNT=0x0A00 /D_WIN32_IE=0x0600 /DJISUQI_EMBEDDED /Iinclude /c main.cpp App.cpp MainWindow.cpp TrayIcon.cpp MouseHook.cpp DragDrop.cpp Config.cpp IconManager.cpp Pinyin.cpp AutoStart.cpp settings_ui.cpp jisuq_ui_layout.cpp
if errorlevel 1 ( echo [ERROR] compile failed & exit /b 1 )

echo [INFO] Compiling resources...
rc.exe /nologo JisuQi.rc
if errorlevel 1 ( echo [ERROR] resource compile failed & exit /b 1 )

echo [INFO] Linking...
link.exe /nologo /SUBSYSTEM:WINDOWS /MACHINE:X64 /OUT:JisuQi.exe main.obj App.obj MainWindow.obj TrayIcon.obj MouseHook.obj DragDrop.obj Config.obj IconManager.obj Pinyin.obj AutoStart.obj settings_ui.obj jisuq_ui_layout.obj JisuQi.res user32.lib gdiplus.lib shell32.lib ole32.lib comctl32.lib gdi32.lib uuid.lib imm32.lib dwmapi.lib shlwapi.lib
if errorlevel 1 ( echo [ERROR] link failed & exit /b 1 )

echo [OK] Built JisuQi.exe
if exist "极速启.exe" del "极速启.exe"
rename JisuQi.exe 极速启.exe
echo [OK] Renamed to 极速启.exe
endlocal
exit /b 0
