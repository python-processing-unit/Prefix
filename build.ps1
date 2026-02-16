<#
build.ps1
Compiles the interpreter in a temporary directory and copies the resulting EXE
back into this repo. Also discovers extension C sources under ext/ and lib/
and compiles each one into a dynamic library next to its source file.

Requires: run from a Developer Command Prompt for Visual Studio where cl.exe is on PATH.
Usage (from Prefix-C folder):
    powershell -ExecutionPolicy Bypass -File .\build.ps1
#>

$script = Split-Path -Parent $MyInvocation.MyCommand.Definition
$src = Join-Path $script "src"
$extRoots = @(
    (Join-Path $script "ext"),
    (Join-Path $script "lib"),
    (Join-Path $script "tests")
)

Write-Host "Preparing temp build directory under`$env:TEMP..."
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$buildDir = Join-Path $env:TEMP ("prefix-build-$stamp")
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
Write-Host "Build dir: $buildDir"

$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $cl) {
    Write-Error "cl.exe not found. Run this script from a Developer Command Prompt for Visual Studio."
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    exit 1
}

$cFiles = Get-ChildItem -Path $src -Filter *.c -File -Recurse | ForEach-Object { $_.FullName }
if ($cFiles.Count -eq 0) {
    Write-Error "No .c files found in '$src'"
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    exit 1
}

$platform = [System.Runtime.InteropServices.RuntimeInformation]
$extSuffix = ".dll"
if ($platform::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Linux)) {
    $extSuffix = ".so"
} elseif ($platform::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::OSX)) {
    $extSuffix = ".dylib"
}

Push-Location $buildDir
try {
    $exeArgs = @(
        "/std:c17", "/Gd", "/O2", "/Gy", "/GF", "/GL", "/W4", "/WX", "/MP", "/nologo",
        "/Fe:prefix.exe"
    )
    $exeArgs += $cFiles

    Write-Host "Invoking: cl.exe $($exeArgs -join ' ')"
    & cl.exe @exeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "cl.exe returned exit code $LASTEXITCODE while building interpreter"
    }

    $outExe = Join-Path $buildDir "prefix.exe"
    if (-not (Test-Path $outExe)) {
        throw "Expected output EXE not found: $outExe"
    }

    $exeDest = Join-Path $script "prefix.exe"
    Copy-Item -Path $outExe -Destination $exeDest -Force
    Write-Host "Copied EXE to: $exeDest"

    $extSources = @()
    foreach ($root in $extRoots) {
        if (Test-Path $root) {
            $extSources += Get-ChildItem -Path $root -Filter *.c -File -Recurse
        }
    }

    if ($extSources.Count -eq 0) {
        Write-Host "No extension C sources found under ext/ or lib/."
    } else {
        Write-Host "Found $($extSources.Count) extension source file(s)."
    }

    foreach ($extSource in $extSources) {
        $extSourcePath = $extSource.FullName
        $extName = [System.IO.Path]::GetFileNameWithoutExtension($extSourcePath)
        $extOutName = "$extName$extSuffix"
        $extDest = Join-Path $extSource.DirectoryName $extOutName
        $extBuildDir = Join-Path $buildDir ("ext-" + [guid]::NewGuid().ToString("N"))

        New-Item -ItemType Directory -Path $extBuildDir -Force | Out-Null
        Push-Location $extBuildDir
        try {
            $extArgs = @(
                "/std:c17", "/Gd", "/O2", "/W4", "/WX", "/nologo", "/LD",
                "/I$src",
                "/Fe:$extOutName",
                $extSourcePath
            )

            Write-Host "Invoking: cl.exe $($extArgs -join ' ')"
            & cl.exe @extArgs
            if ($LASTEXITCODE -ne 0) {
                throw "cl.exe returned exit code $LASTEXITCODE while building extension '$extSourcePath'"
            }

            $extOutPath = Join-Path $extBuildDir $extOutName
            if (-not (Test-Path $extOutPath)) {
                throw "Expected output extension not found: $extOutPath"
            }

            Copy-Item -Path $extOutPath -Destination $extDest -Force
            Write-Host "Copied extension to: $extDest"
        } finally {
            Pop-Location
            Remove-Item -Recurse -Force $extBuildDir -ErrorAction SilentlyContinue
        }
    }
} catch {
    Write-Error "Build failed: $_"
    exit 1
} finally {
    Pop-Location
    Write-Host "Cleaning up temp build dir: $buildDir"
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
}

Write-Host "Build succeeded and exe copied to: $(Join-Path $script 'prefix.exe')"
exit 0
