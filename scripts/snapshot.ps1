#Requires -Version 5.1
<#
.SYNOPSIS
  Snapshot the repo state as a git tag so the watchdog can rollback.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Tag,
    [string]$Message = ""
)
$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot
if (-not (Test-Path '.git')) {
    git init | Out-Null
    git config user.email "ctsc-harness@local"
    git config user.name  "ctsc harness"
}
git add -A | Out-Null
$status = git status --porcelain
if ($status) {
    git commit -m ("snapshot: {0} {1}" -f $Tag, $Message) | Out-Null
}
git tag -f $Tag
Write-Host ("tagged {0}" -f $Tag)
