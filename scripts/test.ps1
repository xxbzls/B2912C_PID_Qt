. "$PSScriptRoot\env.ps1"
$env:TEST_OUTPUT_DIR = Join-Path $projectRoot 'tests\results'
New-Item -ItemType Directory -Force $env:TEST_OUTPUT_DIR | Out-Null
$exe = Join-Path $projectRoot 'release\B2912C_PID_Qt.exe'
foreach ($test in @('--self-test','--sdk-test','--protection-test','--monitor-test','--group-test','--measurement-test','--target-test','--meeting-test','--connection-check','--session-connect-check','--connection-smoke-test','--worker-test','--current-worker-test','--smoke-test','--manual-smoke-test','--plot-test')) {
    $process = Start-Process -FilePath $exe -ArgumentList $test -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "$test failed (exit $($process.ExitCode)); see tests/results" }
    Write-Output "$test PASS"
}
