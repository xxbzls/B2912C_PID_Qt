param([switch]$Deploy)
$ErrorActionPreference='Stop'
$qtPath='D:\Qt\qt5.15.2-mingw64'
$compilerPath='C:\mingw64'
$env:PATH="$qtPath\bin;$compilerPath\bin;"+$env:PATH
$testBuild=Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $testBuild | Out-Null
Push-Location $testBuild
try {
    & "$qtPath\bin\qmake.exe" (Join-Path $PSScriptRoot 'StabilityTest.pro') '-spec' 'win32-g++' 'CONFIG+=release' 'CONFIG-=debug'
    if($LASTEXITCODE -ne 0){throw 'qmake failed'}
    & "$compilerPath\bin\mingw32-make.exe" '-j4'
    if($LASTEXITCODE -ne 0){throw 'Build failed'}
} finally {Pop-Location}
if($Deploy) {
    $destination=Join-Path (Split-Path $PSScriptRoot -Parent) 'B2912C_StabilityTest_v1.0'
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    $destinationExe=Join-Path $destination 'B2912C_StabilityTest.exe'
    if(Get-Process -Name 'B2912C_StabilityTest' -ErrorAction SilentlyContinue){throw 'Close the offline test application before deploying'}
    Copy-Item -LiteralPath (Join-Path $testBuild 'bin\B2912C_StabilityTest.exe') -Destination $destinationExe
    & "$qtPath\bin\windeployqt.exe" '--release' '--no-translations' '--no-opengl-sw' $destinationExe
    if($LASTEXITCODE -ne 0){throw 'Qt deployment failed'}
    foreach($name in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
        Copy-Item -LiteralPath "$compilerPath\bin\$name" -Destination $destination
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '使用说明.txt') -Destination $destination
    New-Item -ItemType Directory -Force -Path (Join-Path $destination 'HisData') | Out-Null
    Write-Output "Deployed: $destinationExe"
}
