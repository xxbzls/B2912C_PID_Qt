$ErrorActionPreference = 'Stop'
if (-not $env:QT_ROOT) { $env:QT_ROOT = 'D:\Qt\qt5.15.2-mingw64' }
if (-not $env:MINGW_ROOT) { $env:MINGW_ROOT = 'C:\mingw64' }
if (-not $env:IVI_ROOT) { $env:IVI_ROOT = 'C:\Program Files\IVI Foundation\IVI' }
foreach ($required in @("$env:QT_ROOT\bin\qmake.exe", "$env:MINGW_ROOT\bin\g++.exe", "$env:IVI_ROOT\Include\KtB2900.h", "$env:IVI_ROOT\Bin\KtB2900_64.dll")) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing dependency: $required" }
}
$env:PATH = "$env:QT_ROOT\bin;$env:MINGW_ROOT\bin;$env:IVI_ROOT\Bin;" + $env:PATH
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$env:QT_ROOT\plugins"
$projectRoot = Split-Path -Parent $PSScriptRoot
