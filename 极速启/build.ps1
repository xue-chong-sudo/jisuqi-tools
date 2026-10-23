# 极速启（启动面板）—— 一键编译
#   用法：powershell -ExecutionPolicy Bypass -File build.ps1
#   产物：dist\极速启.exe
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$msvc = 'D:\Visual Studio 2022\VC\Tools\MSVC\14.51.36231'
$sdk  = 'D:\Windows Kits\10'
$sdkv = '10.0.26100.0'

$env:PATH    = "$msvc\bin\Hostx64\x64;$sdk\bin\$sdkv\x64;" + $env:PATH
$env:INCLUDE = "$msvc\include;$sdk\Include\$sdkv\um;$sdk\Include\$sdkv\shared;$sdk\Include\$sdkv\ucrt;$root\include"
$env:LIB     = "$msvc\lib\x64;$sdk\Lib\$sdkv\um\x64;$sdk\Lib\$sdkv\ucrt\x64"

Set-Location $root
New-Item -ItemType Directory -Force -Path "$root\dist" | Out-Null

Write-Host '[1/3] compiling resources ...'
& "$sdk\bin\$sdkv\x64\rc.exe" /nologo JisuQi.rc
if ($LASTEXITCODE -ne 0) { throw 'rc.exe failed' }

Write-Host '[2/3] compiling sources ...'
$srcs = @('main.cpp','App.cpp','MainWindow.cpp','TrayIcon.cpp','MouseHook.cpp',
          'DragDrop.cpp','Config.cpp','IconManager.cpp','Pinyin.cpp','AutoStart.cpp',
          'settings_ui.cpp','jisuq_ui_layout.cpp')
$clArgs = @('/nologo','/std:c++20','/O2','/MT','/EHsc','/Gy',
            '/utf-8','/D_WIN32_WINNT=0x0A00','/D_WIN32_IE=0x0600','/DJISUQI_EMBEDDED',
            '/Iinclude','/c') + $srcs
& "$msvc\bin\Hostx64\x64\cl.exe" @clArgs
if ($LASTEXITCODE -ne 0) { throw 'cl.exe failed' }

Write-Host '[3/3] linking ...'
$objs = $srcs | ForEach-Object { [IO.Path]::ChangeExtension($_, '.obj') }
$linkArgs = @('/nologo','/SUBSYSTEM:WINDOWS','/MACHINE:X64','/MANIFEST:NO',
              "/OUT:$root\dist\JisuQi.exe") + $objs + @('JisuQi.res','user32.lib',
              'gdiplus.lib','shell32.lib','ole32.lib','comctl32.lib','gdi32.lib',
              'uuid.lib','imm32.lib','dwmapi.lib','shlwapi.lib')
& "$msvc\bin\Hostx64\x64\link.exe" @linkArgs
if ($LASTEXITCODE -ne 0) { throw 'link.exe failed' }

Get-ChildItem "$root\*.obj","$root\*.res" -ErrorAction SilentlyContinue | Remove-Item -Force
# 链接器对中文输出名的编码不稳，先出 ASCII 名再改回来
Move-Item "$root\dist\JisuQi.exe" "$root\dist\极速启.exe" -Force
Write-Host '[OK] dist\极速启.exe'
