@echo off
setlocal
cd /d "%~dp0"

REM Direct MSVC env (same layout as build_static.ps1), no vcvars64/reg.exe.
set "MSVC=D:\Visual Studio 2022\VC\Tools\MSVC\14.51.36231"
set "SDK=D:\Windows Kits\10"
set "SDKV=10.0.26100.0"
set "PATH=%MSVC%\bin\Hostx64\x64;%SDK%\bin\%SDKV%\x64;%PATH%"
set "INCLUDE=%MSVC%\include;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared;%SDK%\Include\%SDKV%\ucrt;%~dp0deps\tesseract\include"
set "LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKV%\um\x64;%SDK%\Lib\%SDKV%\ucrt\x64;%~dp0deps\tesseract\lib"

REM 编译图标资源（在 res 目录下执行，使 app.rc 中的 "app.ico" 相对路径可解析）
pushd "%~dp0res"
rc.exe /nologo /fo "%~dp0res\app.res" "app.rc"
if errorlevel 1 ( echo [ERROR] icon rc failed & popd & exit /b 1 )
popd

echo [INFO] Compiling screenshot-ocr host...
cl.exe /nologo /O2 /EHsc /utf-8 /MD /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
  /I"%~dp0src" /I"%~dp0deps\tesseract\include" ^
  src\main.cpp src\capture.cpp src\view.cpp src\ocr_tesseract.cpp src\ocraddon.cpp "%~dp0res\app.res" ^
  /Fe"%~dp0dist\screenshot_host.exe" ^
  /link user32.lib gdi32.lib gdiplus.lib dwmapi.lib ws2_32.lib shell32.lib advapi32.lib shlwapi.lib comdlg32.lib ^
  ucrt.lib vcruntime.lib msvcrt.lib oldnames.lib ^
  "%~dp0deps\tesseract\lib_static\tesseract55.lib" "%~dp0deps\tesseract\lib\leptonica-1.87.0.lib" ^
  /SUBSYSTEM:WINDOWS /MACHINE:X64 ^
  /out:"%~dp0dist\screenshot_host.exe"
if errorlevel 1 ( echo [ERROR] host build failed & exit /b 1 )

echo [OK] Built dist\screenshot_host.exe
endlocal
exit /b 0
