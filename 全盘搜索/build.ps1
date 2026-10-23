# 全盘搜索（极速启的搜索插件）—— 一键编译
#   用法：powershell -ExecutionPolicy Bypass -File build.ps1
#   产物：dist\全盘搜索.exe
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$msvc = 'D:\Visual Studio 2022\VC\Tools\MSVC\14.51.36231'
$sdk  = 'D:\Windows Kits\10'
$sdkv = '10.0.26100.0'

$env:PATH    = "$msvc\bin\Hostx64\x64;$sdk\bin\$sdkv\x64;" + $env:PATH
$env:INCLUDE = "$msvc\include;$sdk\include\$sdkv\ucrt;$sdk\include\$sdkv\um;$sdk\include\$sdkv\shared"
$env:LIB     = "$msvc\lib\x64;$sdk\lib\$sdkv\ucrt\x64;$sdk\lib\$sdkv\um\x64"

Set-Location $root
New-Item -ItemType Directory -Force -Path "$root\dist" | Out-Null

Write-Host '[1/3] compiling resources ...'
& "$sdk\bin\$sdkv\x64\rc.exe" /nologo /c 65001 /fo app.res app.rc
if ($LASTEXITCODE -ne 0) { throw 'rc.exe failed (app.ico missing? run: python gen_ico.py)' }

Write-Host '[2/3] compiling search_app.cpp ...'
$clArgs = @('/nologo','/std:c++20','/O2','/MT','/EHsc','/utf-8','/W2','/c','search_app.cpp')
& "$msvc\bin\Hostx64\x64\cl.exe" @clArgs
if ($LASTEXITCODE -ne 0) { throw 'cl.exe failed' }

Write-Host '[3/3] linking ...'
$linkArgs = @('/nologo','/SUBSYSTEM:WINDOWS','/OPT:REF','/OPT:ICF',
              "/OUT:$root\dist\newsearch.exe",'search_app.obj','app.res') + @(
              'user32.lib','gdi32.lib','shell32.lib','ole32.lib','comctl32.lib',
              'gdiplus.lib','dwmapi.lib','shlwapi.lib','advapi32.lib')
& "$msvc\bin\Hostx64\x64\link.exe" @linkArgs
if ($LASTEXITCODE -ne 0) { throw 'link.exe failed' }

Get-ChildItem "$root\*.obj","$root\app.res" -ErrorAction SilentlyContinue | Remove-Item -Force
Move-Item "$root\dist\newsearch.exe" "$root\dist\全盘搜索.exe" -Force
Write-Host '[OK] dist\全盘搜索.exe'
