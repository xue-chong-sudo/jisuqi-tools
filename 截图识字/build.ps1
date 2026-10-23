# 截图识字 —— 一键编译
#   用法：powershell -ExecutionPolicy Bypass -File build.ps1 [-Deps <tesseract目录>]
#   产物：dist\截图识字.exe
param([string]$Deps = '')

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

if (-not $Deps) {
    foreach ($c in @("$root\deps\tesseract", 'D:\jisuqi_build\v2shot\deps\tesseract',
                     'D:\jisuqi_build_v2\deps\tesseract', 'D:\bt\tess')) {
        if (Test-Path "$c\lib\tesseract55.lib") { $Deps = $c; break }
    }
}
if (-not $Deps -or -not (Test-Path "$Deps\lib\tesseract55.lib")) {
    Write-Host '[ERROR] tesseract deps not found (need lib\tesseract55.lib). Use -Deps <dir>.'
    exit 1
}

$msvc = 'D:\Visual Studio 2022\VC\Tools\MSVC\14.51.36231'
$sdk  = 'D:\Windows Kits\10'
$sdkv = '10.0.26100.0'

$env:PATH    = "$msvc\bin\Hostx64\x64;$sdk\bin\$sdkv\x64;" + $env:PATH
$env:INCLUDE = "$msvc\include;$sdk\Include\$sdkv\um;$sdk\Include\$sdkv\shared;$sdk\Include\$sdkv\ucrt;$Deps\include"
$env:LIB     = "$msvc\lib\x64;$sdk\Lib\$sdkv\um\x64;$sdk\Lib\$sdkv\ucrt\x64;$Deps\lib"

Set-Location $root
New-Item -ItemType Directory -Force -Path "$root\dist" | Out-Null
Write-Host "[deps] $Deps"

Write-Host '[1/2] compiling icon resource ...'
Push-Location "$root\res"
& "$sdk\bin\$sdkv\x64\rc.exe" /nologo /fo "$root\res\app.res" app.rc
$rc = $LASTEXITCODE
Pop-Location
if ($rc -ne 0) { throw 'rc.exe failed' }

Write-Host '[2/2] compiling and linking ...'
$clArgs = @('/nologo','/std:c++20','/O2','/EHsc','/utf-8','/MD','/DUNICODE','/D_UNICODE',
            '/D_CRT_SECURE_NO_WARNINGS', "/I$root\src", "/I$Deps\include",
            "$root\src\main.cpp","$root\src\capture.cpp","$root\src\view.cpp",
            "$root\src\ocr_tesseract.cpp","$root\src\ocraddon.cpp","$root\res\app.res",
            "/Fe:$root\dist\screenshot_host.exe", '/link',
            'user32.lib','gdi32.lib','gdiplus.lib','dwmapi.lib','ws2_32.lib',
            'shell32.lib','advapi32.lib','shlwapi.lib','comdlg32.lib',
            'ucrt.lib','vcruntime.lib','msvcrt.lib','oldnames.lib',
            "$Deps\lib_static\tesseract55.lib","$Deps\lib\leptonica-1.87.0.lib",
            '/SUBSYSTEM:WINDOWS','/MACHINE:X64')
& "$msvc\bin\Hostx64\x64\cl.exe" @clArgs
if ($LASTEXITCODE -ne 0) { throw 'cl.exe/link failed' }

Get-ChildItem "$root\*.obj","$root\src\*.obj" -ErrorAction SilentlyContinue | Remove-Item -Force
Move-Item "$root\dist\screenshot_host.exe" "$root\dist\截图识字.exe" -Force
Write-Host '[OK] dist\截图识字.exe'
Write-Host '     copy the runtime DLLs (leptonica/jpeg/png/...) and tessdata\ next to it before running'
