<#
build-temp.ps1
Compile the project inside a temporary directory under $env:TEMP (e.g. C:\Windows\Temp), copy the resulting EXE back into this repo, and remove all temporary artifacts.
Requires: run from a Developer Command Prompt for Visual Studio where `cl.exe` is on PATH.
Usage (from Prefix-C folder):
    powershell -ExecutionPolicy Bypass -File .\build-temp.ps1
#>

$script = Split-Path -Parent $MyInvocation.MyCommand.Definition
$src = Join-Path $script "src"

Write-Host "Preparing temp build directory under`$env:TEMP`..."
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$buildDir = Join-Path $env:TEMP ("prefix-build-$stamp")
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

Write-Host "Build dir: $buildDir"

# Ensure cl.exe is available
$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $cl) {
    Write-Error "cl.exe not found. Run this script from a Developer Command Prompt for Visual Studio."
    exit 1
}

# Collect .c files
$cFiles = Get-ChildItem -Path $src -Filter *.c -File -Recurse | ForEach-Object { $_.FullName }
if ($cFiles.Count -eq 0) {
    Write-Error "No .c files found in '$src'"
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    exit 1
}

Push-Location $buildDir

try {
    # Build in temp dir; cl will place outputs in the current dir
    $fe = "/Fe:prefix.exe"
    $args = @("/std:c17", "/Gd", "/O2", "/W4", "/WX", "/nologo", $fe)
    $args += $cFiles

    Write-Host "Invoking: cl.exe $($args -join ' ')"
    & cl.exe @args
    $rc = $LASTEXITCODE
    if ($rc -ne 0) {
        throw "cl.exe returned exit code $rc"
    }

    # Copy EXE back to repo (Prefix-C root)
    $outExe = Join-Path $buildDir 'prefix.exe'
    if (-not (Test-Path $outExe)) {
        throw "Expected output EXE not found: $outExe"
    }
    $dest = Join-Path $script 'prefix.exe'
    Copy-Item -Path $outExe -Destination $dest -Force
    Write-Host "Copied EXE to: $dest"

} catch {
    Write-Error "Build failed: $_"
    Pop-Location
    # Cleanup temp build dir on failure as well
    Write-Host "Cleaning up temp build dir: $buildDir"
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    exit 1
} finally {
    Pop-Location
}

# Cleanup temp build dir
Write-Host "Cleaning up temp build dir: $buildDir"
Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue

Write-Host "Build succeeded and exe copied to: $(Join-Path $script 'prefix.exe')"
exit 0
