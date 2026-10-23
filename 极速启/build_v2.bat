@echo off
setlocal
cd /d "%~dp0"

REM Direct MSVC env (same layout as the screenshot-ocr build_static.ps1),
REM avoids vcvars64.bat which shells out to reg.exe.
set "MSVC=D:\Visual Studio 2022\VC\Tools\MSVC\14.51.36231"
set "SDK=D:\Windows Kits\10"
set "SDKV=10.0.26100.0"
set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKV%\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\winrt"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKV%\um\x64;%SDK%\Lib\%SDKV%\ucrt\x64"

where cl.exe
where rc.exe

echo [INFO] Compiling jisuqi...
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
