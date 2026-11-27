# Elevated command broker (test machine only). Started once with UAC consent; runs what is dropped into queue\.
#
#   queue\<id>.json   request: { "id": "...", "command": "...", "shell": "pwsh"|"cmd", "cwd": "...", "timeout_s": N }
#                     or      { "id": "...", "kill": "<id of a running request>" }
#   done\<id>.json    result:  { id, exit_code, timed_out, killed, pid, started, ended, seconds, stdout, stderr }
#   done\<id>.out / .err       the command's output
#   broker.json       { pid, admin, started, heartbeat, running: [...] }, rewritten every second
#   broker.log        every request, with its command (the audit trail)
#   stop              create this file to end the broker (running commands are left alone)
#
# Use elevated.py next to this file; do not write requests by hand.
param([string]$Root = $(if ($env:MCLA_ELEVATE_DIR) { $env:MCLA_ELEVATE_DIR } elseif ($env:MCLA_WORK) { Join-Path $env:MCLA_WORK 'elevate' } else { Join-Path (Split-Path (Split-Path (Split-Path $PSScriptRoot))) 'mcla-x86-work\elevate' }))
New-Item -ItemType Directory -Force $Root | Out-Null

$ErrorActionPreference = 'Continue'
$queue = Join-Path $Root 'queue'
$done = Join-Path $Root 'done'
$work = Join-Path $Root 'work'
New-Item -ItemType Directory -Force $queue, $done, $work | Out-Null
$logFile = Join-Path $Root 'broker.log'
$stateFile = Join-Path $Root 'broker.json'
$stopFile = Join-Path $Root 'stop'

function Write-Log([string]$message) {
  Add-Content -Path $logFile -Value ("{0} {1}" -f (Get-Date -Format o), $message) -Encoding utf8
}

function Write-Atomic([string]$path, [string]$text) {
  $tmp = "$path.tmp"
  [IO.File]::WriteAllText($tmp, $text, [Text.UTF8Encoding]::new($false))
  Move-Item -LiteralPath $tmp -Destination $path -Force
}

function Write-Result([string]$id, [hashtable]$result) {
  Write-Atomic (Join-Path $done "$id.json") ($result | ConvertTo-Json -Compress)
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$started = Get-Date
$running = @{}
if (Test-Path $stopFile) { Remove-Item $stopFile -Force }
Write-Log "broker start pid $PID user $($identity.Name) admin=$isAdmin root $Root"

$lastBeat = [datetime]::MinValue
while ($true) {
  if (Test-Path $stopFile) {
    Remove-Item $stopFile -Force
    break
  }

  if (((Get-Date) - $lastBeat).TotalSeconds -ge 1) {
    $lastBeat = Get-Date
    $state = @{
      pid = $PID; admin = $isAdmin; user = $identity.Name
      started = $started.ToString('o'); heartbeat = $lastBeat.ToString('o')
      running = @($running.Keys)
    }
    try { Write-Atomic $stateFile ($state | ConvertTo-Json -Compress) } catch {}
  }

  foreach ($file in @(Get-ChildItem -LiteralPath $queue -Filter '*.json' -File -ErrorAction SilentlyContinue)) {
    $req = $null
    try { $req = Get-Content -LiteralPath $file.FullName -Raw -Encoding utf8 | ConvertFrom-Json } catch {}
    Remove-Item -LiteralPath $file.FullName -Force -ErrorAction SilentlyContinue
    if (-not $req -or -not ($req.id -match '^[A-Za-z0-9_.-]{1,64}$')) {
      Write-Log "bad request file $($file.Name)"
      continue
    }
    $id = [string]$req.id

    if ($req.kill) {
      $target = [string]$req.kill
      if ($running.ContainsKey($target)) {
        $running[$target].killed = $true
        & taskkill.exe /T /F /PID $running[$target].process.Id 2>&1 | Out-Null
        Write-Log "kill $target (asked by $id)"
        Write-Result $id @{ id = $id; exit_code = 0; killed_target = $target }
      } else {
        Write-Result $id @{ id = $id; exit_code = 1; error = "no running request '$target'" }
      }
      continue
    }

    $command = [string]$req.command
    if (-not $command) {
      Write-Result $id @{ id = $id; exit_code = 1; error = 'empty command' }
      continue
    }
    $shell = if ($req.shell -eq 'cmd') { 'cmd' } else { 'pwsh' }
    $cwd = if ($req.cwd -and (Test-Path -LiteralPath ([string]$req.cwd) -PathType Container)) { [string]$req.cwd } else { $Root }
    $timeout = 0
    try { $timeout = [int]$req.timeout_s } catch {}
    $outFile = Join-Path $done "$id.out"
    $errFile = Join-Path $done "$id.err"
    try {
      if ($shell -eq 'cmd') {
        $script = Join-Path $work "$id.cmd"
        [IO.File]::WriteAllText($script, "@echo off`r`n$command`r`n", [Text.UTF8Encoding]::new($false))
        $inner = "call `"$script`""
      } else {
        $script = Join-Path $work "$id.ps1"
        [IO.File]::WriteAllText($script, "$command`r`nexit `$LASTEXITCODE`r`n", [Text.UTF8Encoding]::new($true))
        $inner = "pwsh.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$script`""
      }
      $psi = [Diagnostics.ProcessStartInfo]::new()
      $psi.FileName = "$env:SystemRoot\System32\cmd.exe"
      $psi.Arguments = "/d /s /c `"$inner > `"$outFile`" 2> `"$errFile`" < NUL`""
      $psi.WorkingDirectory = $cwd
      $psi.UseShellExecute = $false
      $psi.CreateNoWindow = $true
      $process = [Diagnostics.Process]::Start($psi)
      $running[$id] = @{
        process = $process; started = Get-Date; killed = $false; timed_out = $false
        deadline = if ($timeout -gt 0) { (Get-Date).AddSeconds($timeout) } else { $null }
        out = $outFile; err = $errFile
      }
      Write-Log "run $id pid $($process.Id) shell=$shell cwd=$cwd timeout=$timeout :: $($command -replace '\s+', ' ')"
    } catch {
      Write-Log "run $id FAILED to start: $_"
      Write-Result $id @{ id = $id; exit_code = 1; error = "could not start: $_" }
    }
  }

  foreach ($id in @($running.Keys)) {
    $entry = $running[$id]
    if ($entry.process.HasExited) {
      $ended = Get-Date
      Write-Result $id @{
        id = $id; exit_code = $entry.process.ExitCode; timed_out = $entry.timed_out; killed = $entry.killed
        pid = $entry.process.Id; started = $entry.started.ToString('o'); ended = $ended.ToString('o')
        seconds = [math]::Round(($ended - $entry.started).TotalSeconds, 2); stdout = $entry.out; stderr = $entry.err
      }
      Write-Log "done $id exit $($entry.process.ExitCode) timed_out=$($entry.timed_out) killed=$($entry.killed)"
      $running.Remove($id)
    } elseif ($entry.deadline -and (Get-Date) -gt $entry.deadline -and -not $entry.timed_out) {
      $entry.timed_out = $true
      & taskkill.exe /T /F /PID $entry.process.Id 2>&1 | Out-Null
      Write-Log "timeout $id"
    }
  }

  Start-Sleep -Milliseconds 200
}

Write-Log "broker stop pid $PID ($($running.Count) command(s) still running, left alone)"
Remove-Item -LiteralPath $stateFile -Force -ErrorAction SilentlyContinue
