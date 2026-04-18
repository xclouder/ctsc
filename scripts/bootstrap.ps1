#Requires -Version 5.1
<#
.SYNOPSIS
  Bootstrap ctsc harness environment: detect toolchain, clone TypeScript, build tsc oracle, install harness deps.

.DESCRIPTION
  Safe to run repeatedly. Checks what is already present before doing work.
  Does NOT auto-install compilers (Windows policy). Prints guidance if missing.

.PARAMETER SkipClone
  Skip cloning / updating upstream/TypeScript.

.PARAMETER SkipHarness
  Skip `npm install` inside harness/.

.PARAMETER SkipTscBuild
  Skip building upstream tsc (built/local/tsc.js).
#>

[CmdletBinding()]
param(
    [switch]$SkipClone,
    [switch]$SkipHarness,
    [switch]$SkipTscBuild
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

function Test-Command {
    param([string]$Name)
    $null = Get-Command $Name -ErrorAction SilentlyContinue
    return $?
}

function Write-Step {
    param([string]$Msg)
    Write-Host ""
    Write-Host "==> $Msg" -ForegroundColor Cyan
}

function Write-Ok   { param([string]$Msg) Write-Host "    [ok]   $Msg" -ForegroundColor Green }
function Write-Miss { param([string]$Msg) Write-Host "    [miss] $Msg" -ForegroundColor Yellow }
function Write-Bad  { param([string]$Msg) Write-Host "    [bad]  $Msg" -ForegroundColor Red }

Write-Step "Detecting toolchain"

$hasNode   = Test-Command node
$hasNpm    = Test-Command npm
$hasGit    = Test-Command git
$hasCmake  = Test-Command cmake
$hasNinja  = Test-Command ninja
$hasClang  = Test-Command clang
$hasGcc    = Test-Command gcc
$hasCl     = Test-Command cl
$hasCursor = Test-Command cursor-agent

if ($hasNode)  { Write-Ok   ("node  : {0}" -f (node --version)) }       else { Write-Bad  "node missing" }
if ($hasNpm)   { Write-Ok   ("npm   : {0}" -f (npm --version)) }        else { Write-Bad  "npm missing" }
if ($hasGit)   { Write-Ok   ("git   : {0}" -f ((git --version) -replace '^git version ','')) } else { Write-Bad "git missing" }
if ($hasCmake) { Write-Ok   ("cmake : {0}" -f (((cmake --version) -split "`n")[0])) } else { Write-Miss "cmake missing" }
if ($hasNinja) { Write-Ok   ("ninja : {0}" -f (ninja --version)) }       else { Write-Miss "ninja missing" }
if ($hasClang) { Write-Ok   ("clang : detected") }                       else { Write-Miss "clang missing" }
if ($hasGcc)   { Write-Ok   ("gcc   : detected") }                       else { Write-Miss "gcc missing" }
if ($hasCl)    { Write-Ok   ("cl    : detected (MSVC)") }                else { Write-Miss "cl missing (run from a Developer PowerShell to expose MSVC)" }
if ($hasCursor){ Write-Ok   ("cursor-agent : detected") }                else { Write-Miss "cursor-agent not on PATH (agent loop will be disabled)" }

if (-not $hasNode -or -not $hasNpm -or -not $hasGit) {
    throw "Core prerequisites missing. Install Node.js LTS and Git for Windows."
}

if (-not ($hasCl -or $hasClang -or $hasGcc)) {
    Write-Host ""
    Write-Host "No C compiler detected. Install ONE of:" -ForegroundColor Yellow
    Write-Host "  Option A (recommended on Windows): Visual Studio 2022 Build Tools with 'Desktop development with C++'"
    Write-Host "    Then launch from: Start Menu -> Visual Studio 2022 -> 'Developer PowerShell for VS 2022'"
    Write-Host "  Option B: LLVM/clang for Windows (winget install LLVM.LLVM) + Ninja (winget install Ninja-build.Ninja) + CMake (winget install Kitware.CMake)"
    Write-Host "Harness will still install, but ctsc build steps will be skipped until a compiler is available." -ForegroundColor Yellow
}

if (-not $SkipClone) {
    Write-Step "Cloning upstream/TypeScript (shallow)"
    $upstream = Join-Path $RepoRoot 'upstream\TypeScript'
    if (-not (Test-Path $upstream)) {
        New-Item -ItemType Directory -Force -Path (Join-Path $RepoRoot 'upstream') | Out-Null
        git clone --depth=1 https://github.com/microsoft/TypeScript.git $upstream
    } else {
        Write-Ok "upstream/TypeScript already exists"
    }
}

if (-not $SkipTscBuild) {
    Write-Step "Installing upstream TypeScript deps (for tsc oracle)"
    $upstream = Join-Path $RepoRoot 'upstream\TypeScript'
    if (Test-Path $upstream) {
        Push-Location $upstream
        try {
            if (-not (Test-Path 'node_modules')) {
                npm install --no-audit --no-fund --loglevel=error
            } else {
                Write-Ok "upstream node_modules already present"
            }
        } finally {
            Pop-Location
        }
    }
}

if (-not $SkipHarness) {
    Write-Step "Installing harness dependencies"
    $harness = Join-Path $RepoRoot 'harness'
    if (Test-Path (Join-Path $harness 'package.json')) {
        Push-Location $harness
        try {
            npm install --no-audit --no-fund --loglevel=error
        } finally {
            Pop-Location
        }
    } else {
        Write-Miss "harness/package.json not found, skipping"
    }
}

Write-Step "Done"
Write-Host "Next:" -ForegroundColor Cyan
Write-Host "  1. From harness/ : npm run loop -- --dry  (sanity-check the pipeline without an agent)"
Write-Host "  2. From harness/ : npm run loop            (drive the agent loop)"
