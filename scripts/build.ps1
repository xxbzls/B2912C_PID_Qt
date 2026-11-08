param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)
. "$PSScriptRoot\env.ps1"
if ($Configuration -eq 'Debug') {
    $buildRoot = Join-Path $projectRoot 'build-qtcreator\Debug'
    $outputDir = Join-Path $projectRoot 'release\debug'
    $qmakeConfiguration = @('CONFIG-=release', 'CONFIG+=debug')
} else {
    $buildRoot = Join-Path $projectRoot 'build-native'
    $outputDir = Join-Path $projectRoot 'release'
    $qmakeConfiguration = @('CONFIG-=debug', 'CONFIG+=release')
}
New-Item -ItemType Directory -Force -Path $buildRoot,$outputDir | Out-Null
$outputExe = Join-Path $outputDir 'B2912C_PID_Qt.exe'
Push-Location $buildRoot
try {
    & "$env:QT_ROOT\bin\qmake.exe" (Join-Path $projectRoot 'B2912C_PID_Qt.pro') '-spec' 'win32-g++' @qmakeConfiguration
    if ($LASTEXITCODE -ne 0) { throw 'qmake failed' }
    & "$env:MINGW_ROOT\bin\mingw32-make.exe" '-j4'
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
    if (-not (Test-Path -LiteralPath $outputExe)) { throw "Build output not found: $outputExe" }
    Write-Output "Built ${Configuration}: $outputExe"
} finally { Pop-Location }
