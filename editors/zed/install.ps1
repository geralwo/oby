# Install the OBY extension for Zed: copy `lspaddon` into Zed's extensions
# folder, replacing whatever is there.
#
# Only files are copied. The language server's launch command lives in Zed's
# settings.json, and Zed has to be reloaded (`zed: extensions`, or a restart) to
# notice anything placed here. See README.md for both.
#
#   .\install.ps1
#   .\install.ps1 -ZedExtensions "D:\elsewhere\Zed\extensions\installed"
#
# If Windows refuses to run it, the execution policy is in the way:
#
#   powershell -ExecutionPolicy Bypass -File install.ps1

[CmdletBinding()]
param(
    [string]$ZedExtensions
)

$ErrorActionPreference = "Stop"

if (-not $ZedExtensions) {
    if (-not $env:LOCALAPPDATA) {
        Write-Error "LOCALAPPDATA is not set -- pass -ZedExtensions <Zed's extensions\installed>"
        exit 1
    }
    $ZedExtensions = Join-Path $env:LOCALAPPDATA "Zed\extensions\installed"
}

$source = Join-Path $PSScriptRoot "lspaddon"
$target = Join-Path $ZedExtensions "lspaddon"

if (-not (Test-Path -LiteralPath $source)) {
    Write-Error "there is no lspaddon folder next to this script ($source)"
    exit 1
}

# extension.wasm is fetched rather than committed, so a fresh clone lacks it.
if (-not (Test-Path -LiteralPath (Join-Path $source "extension.wasm"))) {
    Write-Error "lspaddon\extension.wasm is missing -- fetch it first (see README.md)"
    exit 1
}

# Copy-Item -Recurse nests into an existing folder rather than replacing it, so
# the destination goes first.
if (Test-Path -LiteralPath $target) {
    Remove-Item -LiteralPath $target -Recurse -Force
}

New-Item -ItemType Directory -Path $target -Force | Out-Null
Copy-Item -Path (Join-Path $source "*") -Destination $target -Recurse -Force

Write-Host "copied to $target"
Get-ChildItem -LiteralPath $target -Recurse -File |
    ForEach-Object { Write-Host ("  " + $_.FullName.Substring($target.Length + 1)) }
Write-Host "now reload extensions in Zed: 'zed: extensions'"
