<#
.SYNOPSIS
  Launch N per-phase ctsc harness loops in parallel, each in its own
  build dir + progress file so they don't step on each other.

.DESCRIPTION
  This is the parallel sibling of run-loop.ps1. Each worker:
    - runs `npm run loop -- --phase <P> --forever --build ...`
    - uses ctsc/build/phase-<P>/ as its ninja build tree
    - persists progress to harness/state/progress.<P>.json
    - streams output to harness/reports/loop-<stamp>-<P>.log
    - is launched as a detached cmd.exe so its entire descendant tree
      (node, tsx, agent CLI) can be killed with `taskkill /T /F`

  Use `status` aggregation to see global progress:
      cd harness
      npx tsx src/cli.ts status --all

.PARAMETER Phases
  Phases to run. Default: scanner, parser, binder, checker, emitter.
  Pass a subset to spare CPU/tokens, e.g. -Phases scanner,parser.

.PARAMETER Model
  Primary agent model. Default 'composer-2' (fast + cheap). Upgrades to
  -FallbackModel for fixtures the primary can't crack.

.PARAMETER FallbackModel
  Model to escalate to after -FallbackAfter consecutive no-progress
  attempts on a fixture. Default 'claude-opus-4-7-xhigh'. Set to '' to
  disable escalation (primary only).

.PARAMETER FallbackAfter
  How many consecutive no-progress attempts to burn on the primary
  before escalating. Default 2 (composer gets 2 shots, opus gets 3,
  then watchdog defers).

.PARAMETER RetryDeferred
  Clear deferred flag on all non-passed fixtures before starting.

.PARAMETER StatusEvery
  Each worker prints a status line every N iterations. Default 20.

.PARAMETER Max
  Max iterations per worker. 0 or omit = forever.

.PARAMETER TimeoutSec
  Per-agent-call timeout. Default 1200 (20 min).

.PARAMETER Compiler
  CMAKE_C_COMPILER value propagated to each worker's build. Default:
  honour CC env, else 'clang'. Each worker needs this because cmake
  must configure its own build tree from scratch.

.EXAMPLE
  # Default: composer-2 primary, Opus-4.7 fallback after 2 stuck attempts.
  .\scripts\run-loops.ps1

.EXAMPLE
  # Only fight the scanner + parser backlog (default tiered models).
  .\scripts\run-loops.ps1 -Phases scanner,parser

.EXAMPLE
  # Pure composer-2, no escalation (maximum token thrift).
  .\scripts\run-loops.ps1 -FallbackModel ''

.EXAMPLE
  # Burn Opus on everything (old behaviour before tiered mode).
  .\scripts\run-loops.ps1 -Model claude-opus-4-7-xhigh -FallbackModel ''

.EXAMPLE
  # Retry everything that's currently deferred.
  .\scripts\run-loops.ps1 -RetryDeferred
#>

[CmdletBinding()]
param(
  [string[]] $Phases = @("scanner", "parser", "binder", "checker", "emitter"),
  [string]   $Model         = "composer-2",
  [string]   $FallbackModel = "claude-opus-4-7-xhigh",
  [int]      $FallbackAfter = 2,
  [switch]   $RetryDeferred,
  [int]      $StatusEvery = 20,
  [int]      $Max = 0,
  [int]      $TimeoutSec = 1200,
  [string]   $Compiler = ""
)

$ErrorActionPreference = "Stop"

$root    = Resolve-Path (Join-Path $PSScriptRoot "..")
$harness = Join-Path $root "harness"
$reports = Join-Path $harness "reports"
$state   = Join-Path $harness "state"
New-Item -ItemType Directory -Force -Path $reports | Out-Null
New-Item -ItemType Directory -Force -Path $state   | Out-Null

# Sanity: agent CLI present?
$agent = Get-Command agent -ErrorAction SilentlyContinue
if (-not $agent) {
  Write-Error "Cursor CLI 'agent' not on PATH. Install:  irm 'https://cursor.com/install?win32=true' | iex"
  exit 1
}

# Sanity: cmake present?
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
  Write-Error "cmake not on PATH. Install LLVM/clang + Ninja + CMake first (see scripts\bootstrap.ps1)."
  exit 1
}

$stamp    = Get-Date -Format "yyyyMMdd-HHmmss"
$pidsFile = Join-Path $reports "loops.pids"
"# ctsc parallel loops started $stamp" | Set-Content -Path $pidsFile -Encoding ASCII

# Emit a small stop-all helper next to the pidsfile.
$stopScript = Join-Path $reports "loops.stop.ps1"
@"
# Auto-generated stop-all script for the loops started at $stamp.
`$pids = Get-Content -Path "$pidsFile" | Where-Object { `$_ -notmatch '^#' } | ForEach-Object { `$_.Trim().Split()[0] } | Where-Object { `$_ }
foreach (`$p in `$pids) {
  Write-Host "taskkill /PID `$p /T /F"
  taskkill /PID `$p /T /F 2>&1 | Out-Host
}
Remove-Item -Path "$pidsFile" -ErrorAction SilentlyContinue
Remove-Item -Path "`$MyInvocation.MyCommand.Path" -ErrorAction SilentlyContinue
"@ | Set-Content -Path $stopScript -Encoding UTF8

Write-Host "[run-loops] phases   : $($Phases -join ', ')"
Write-Host "[run-loops] harness  : $harness"
Write-Host "[run-loops] reports  : $reports"
Write-Host "[run-loops] pids     : $pidsFile"
Write-Host "[run-loops] stop all : powershell -File '$stopScript'"
Write-Host ""

foreach ($phase in $Phases) {
  $logPath = Join-Path $reports "loop-$stamp-$phase.log"
  $loopArgs = @(
    "run", "loop", "--",
    "--phase", $phase,
    "--build",
    "--status-every", "$StatusEvery",
    "--timeout", "$TimeoutSec"
  )
  if ($Max -le 0) { $loopArgs += "--forever" } else { $loopArgs += @("--max", "$Max") }
  if ($RetryDeferred) { $loopArgs += "--retry-deferred" }
  if ($Model)         { $loopArgs += @("--model", $Model) }
  if ($FallbackModel) { $loopArgs += @("--fallback-model", $FallbackModel, "--fallback-after", "$FallbackAfter") }

  # cmd.exe /c so `taskkill /T /F` cleans the whole npm->node->tsx->agent tree.
  $cmdArgs = "/c npm " + ($loopArgs -join " ")

  # Propagate compiler preference to this worker's build config without
  # forcing it on the others (each worker has its own ctsc/build/phase-X).
  $envOverride = @{}
  if ($Compiler) { $envOverride["CMAKE_C_COMPILER"] = $Compiler }

  Write-Host "[run-loops] $phase -> $logPath"
  Write-Host "[run-loops]   npm $($loopArgs -join ' ')"

  $proc = Start-Process -FilePath "cmd.exe" `
    -ArgumentList $cmdArgs `
    -WorkingDirectory $harness `
    -RedirectStandardOutput $logPath `
    -RedirectStandardError  "$logPath.err" `
    -PassThru -WindowStyle Hidden

  "$($proc.Id) $phase $logPath" | Add-Content -Path $pidsFile -Encoding ASCII

  # Small stagger so the 5 cmake configure steps don't hammer the FS in
  # exact lockstep (reduces transient "stale lock" pains on slow disks).
  Start-Sleep -Milliseconds 500
}

Write-Host ""
Write-Host "[run-loops] ---- started ----"
Get-Content $pidsFile | Where-Object { $_ -notmatch '^#' } | ForEach-Object { "  $_" } | Out-Host
Write-Host ""
Write-Host "[run-loops] tail any phase:"
Write-Host "  Get-Content -Wait -Tail 40 '$reports\loop-$stamp-<phase>.log'"
Write-Host ""
Write-Host "[run-loops] aggregate progress:"
Write-Host "  cd '$harness'; npx tsx src/cli.ts status --all"
Write-Host ""
Write-Host "[run-loops] stop all:"
Write-Host "  powershell -File '$stopScript'"
