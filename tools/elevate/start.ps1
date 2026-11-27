# Starts the elevated command broker (tools/elevate/broker.ps1). Shows ONE UAC prompt: the owner approves it once, and
# agents can then run commands as administrator through tools/elevate/elevated.py until reboot, logoff or `--stop`.
# Only for a test machine: while it runs, any program of this user can run administrator commands through it.
param([string]$Root = '')
$arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'broker.ps1'))
if ($Root) { $arguments += @('-Root', $Root) }
Start-Process -FilePath (Get-Command pwsh).Source -Verb RunAs -WindowStyle Hidden -ArgumentList $arguments
"broker launch requested: check with  python tools/elevate/elevated.py --status"
