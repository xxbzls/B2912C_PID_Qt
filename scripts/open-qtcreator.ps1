. "$PSScriptRoot\env.ps1"
$creator = 'C:\Qt\qtcreator-12.0.2\bin\qtcreator.exe'
if (-not (Test-Path $creator)) { throw "Set Qt Creator path in this script: $creator" }
$projectFile = Join-Path $projectRoot 'B2912C_PID_Qt.pro'
Write-Output "Qt Creator source: $projectFile"
Start-Process -FilePath $creator -ArgumentList ('"' + $projectFile + '"') -WorkingDirectory $projectRoot
