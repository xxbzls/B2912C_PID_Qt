. "$PSScriptRoot\env.ps1"
Start-Process -FilePath (Join-Path $projectRoot 'release\B2912C_PID_Qt.exe') -WorkingDirectory $projectRoot
