<#
.SYNOPSIS
  Launch the ctsc harness long-running loop in the background.

.DESCRIPTION
  Prereqs:
    1. Cursor CLI installed: irm 'https://cursor.com/install?win32=true' | iex
    2. Authenticated: run `agent login` once, OR set $env:CURSOR_API_KEY
    3. ctsc built: cmake --preset clang && cmake --build ctsc\build\clang

  What this does:
    - Starts `npm run loop` in the background (detached)
    - Streams output to reports\loop-<stamp>.log and reports\loop-latest.log
    - Returns the PID so you can kill it later (Stop-Process -Id <pid>)

.PARAMETER Model
  Agent model. Default 'auto'. Examples: 'composer-2', 'sonnet-4', 'gpt-5'.

.PARAMETER RetryDeferred
  Clear the deferred flag on all non-passed fixtures before starting.

.PARAMETER StatusEvery
  Print a total/passed/failed/deferred line every N iterations.

.PARAMETER Max
  Max iterations. 0 or omit = forever.
#>

[CmdletBinding()]
param(
  [string] $Model = "",
  [switch] $RetryDeferred,
  [int]    $StatusEvery = 20,
  [int]    $Max = 0,
  [int]    $TimeoutSec = 1200
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$harness = Join-Path $root "harness"
$reports = Join-Path $harness "reports"
New-Item -ItemType Directory -Force -Path $reports | Out-Null

# Sanity: agent CLI present?
$agent = Get-Command agent -ErrorAction SilentlyContinue
if (-not $agent) {
  Write-Error "Cursor CLI 'agent' not on PATH. Install:  irm 'https://cursor.com/install?win32=true' | iex"
  exit 1
}

# Sanity: ctsc built?
$ctscExe = @(
  "$root\ctsc\build\default\ctsc.exe",
  "$root\ctsc\build\clang\ctsc.exe",
  "$root\ctsc\build\msvc\ctsc.exe",
  "$root\ctsc\build\release\ctsc.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $ctscExe) {
  Write-Warning "ctsc.exe not found; the loop will (re)build it via --build. Make sure cmake/ninja/clang are on PATH."
}

$stamp   = Get-Date -Format "yyyyMMdd-HHmmss"
$logPath = Join-Path $reports "loop-$stamp.log"
$latest  = Join-Path $reports "loop-latest.log"

$loopArgs = @("run", "loop", "--", "--build", "--status-every", "$StatusEvery", "--timeout", "$TimeoutSec")
if ($Max -le 0) { $loopArgs += "--forever" } else { $loopArgs += @("--max", "$Max") }
if ($RetryDeferred) { $loopArgs += "--retry-deferred" }
if ($Model) { $loopArgs += @("--model", $Model) }

Write-Host "[run-loop] harness : $harness"
Write-Host "[run-loop] log     : $logPath"
Write-Host "[run-loop] args    : npm $($loopArgs -join ' ')"

# Detach via Start-Process. Output is merged stdout+stderr into the log file.
# We intentionally start `cmd.exe /c npm ...` so that the whole descendant
# tree (node, tsx, agent CLI, its node runtime) can be torn down in one shot
# by killing the cmd.exe with taskkill /T.
$cmdArgs = "/c npm " + ($loopArgs -join " ")
$proc = Start-Process -FilePath "cmd.exe" `
  -ArgumentList $cmdArgs `
  -WorkingDirectory $harness `
  -RedirectStandardOutput $logPath `
  -RedirectStandardError  "$logPath.err" `
  -PassThru -WindowStyle Hidden

# Maintain a stable pointer for easy tailing.
Copy-Item $logPath $latest -Force

# Persist the PID so the user can stop it without re-discovering.
$pidFile = Join-Path $reports "loop.pid"
"$($proc.Id)" | Set-Content -Path $pidFile -Encoding ASCII

Write-Host "[run-loop] started PID=$($proc.Id)"
Write-Host "[run-loop] log:       $logPath"
Write-Host "[run-loop] tail:      Get-Content -Wait -Tail 40 '$logPath'"
Write-Host "[run-loop] status:    cd '$harness'; npm run status"
Write-Host "[run-loop] stop:      taskkill /PID $($proc.Id) /T /F"
Write-Host "[run-loop] pid-file:  $pidFile"
