param([string]$Destination = 'dist')
. "$PSScriptRoot\env.ps1"
$dist = if ([System.IO.Path]::IsPathRooted($Destination)) { $Destination } else { Join-Path $projectRoot $Destination }
$dist = [System.IO.Path]::GetFullPath($dist)
New-Item -ItemType Directory -Force $dist | Out-Null
$deployedExe = Join-Path $dist 'B2912C_PID.exe'
$legacyExe = Join-Path $dist 'B2912C_PID_Qt.exe'
$runningDeployment = Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -in @($deployedExe, $legacyExe) }
if ($runningDeployment) { throw 'Destination application is running. Close it after stopping outputs, or deploy to another directory.' }
Copy-Item -LiteralPath (Join-Path $projectRoot 'release\B2912C_PID_Qt.exe') -Destination $deployedExe
Copy-Item -LiteralPath (Join-Path $projectRoot 'assets\probe-control.ico') -Destination (Join-Path $dist 'probe-control.ico')
# Refresh the original filename too if an older deployment still contains it.
if (Test-Path -LiteralPath $legacyExe) { Copy-Item -LiteralPath $deployedExe -Destination $legacyExe }
& "$env:QT_ROOT\bin\windeployqt.exe" '--release' '--no-translations' '--no-opengl-sw' $deployedExe
if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed' }
foreach($runtime in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
    Copy-Item -LiteralPath "$env:MINGW_ROOT\bin\$runtime" -Destination $dist
}
Write-Output "Deployed: $dist (IVI driver still requires its official installer)"
