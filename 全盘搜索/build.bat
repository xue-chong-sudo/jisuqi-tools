@echo off
setlocal
cd /d "%~dp0"

set "VS=D:\Visual Studio 2022"
set "SDK=D:\Windows Kits\10"
set "MSVCVER=14.51.36231"
set "SDKVER=10.0.26100.0"
set "PY=C:\Users\Administrator\.workbuddy\binaries\python\envs\default\Scripts\python.exe"

set "PATH=%VS%\VC\Tools\MSVC\%MSVCVER%\bin\Hostx64\x64;%SDK%\bin\%SDKVER%\x64;%PATH%"
set "INCLUDE=%VS%\VC\Tools\MSVC\%MSVCVER%\include;%SDK%\include\%SDKVER%\ucrt;%SDK%\include\%SDKVER%\um;%SDK%\include\%SDKVER%\shared"
set "LIB=%VS%\VC\Tools\MSVC\%MSVCVER%\lib\x64;%SDK%\lib\%SDKVER%\ucrt\x64;%SDK%\lib\%SDKVER%\um\x64"

if not exist app.ico (
  echo [build] generating app.ico ...
  "%PY%" gen_ico.py
  if errorlevel 1 ( echo [build] icon generate failed & exit /b 1 )
)

echo [build] resources ...
"%SDK%\bin\%SDKVER%\x64\rc.exe" /nologo /c 65001 /fo app.res app.rc
if errorlevel 1 exit /b 1

echo [build] compile ...
cl /nologo /std:c++20 /O2 /MT /EHsc /utf-8 /W2 /c search_app.cpp
if errorlevel 1 exit /b 1

echo [build] link ...
link /nologo /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /OUT:"newsearch.exe" search_app.obj app.res user32.lib gdi32.lib shell32.lib ole32.lib comctl32.lib gdiplus.lib dwmapi.lib shlwapi.lib advapi32.lib
if errorlevel 1 exit /b 1

echo [build] OK
endlocal
