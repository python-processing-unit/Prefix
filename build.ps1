<#
build.ps1
Compiles the Prefix runtime into a shared DLL, links the interpreter EXE
against that DLL's import library, and compiles each discovered extension
against the same shared runtime.

Requires: run from a Developer Command Prompt for Visual Studio where cl.exe is on PATH.
Usage (from Prefix folder):
    powershell -ExecutionPolicy Bypass -File .\build.ps1
#>

$script = Split-Path -Parent $MyInvocation.MyCommand.Definition
$src = Join-Path $script "src"
$runtimeDef = Join-Path $src "prefix_runtime.def"
$runtimeDllName = "prefix_runtime.dll"
$runtimeLibName = "prefix_runtime.lib"
$runtimePdbName = "prefix_runtime.pdb"
$runtimeDllDest = Join-Path $script $runtimeDllName
$runtimeLibDest = Join-Path $script $runtimeLibName
$runtimePdbDest = Join-Path $script $runtimePdbName
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

if (-not (Test-Path $runtimeDef)) {
    Write-Error "Runtime export definition not found: $runtimeDef"
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    exit 1
}

$mainSource = Join-Path $src "main.c"
if (-not (Test-Path $mainSource)) {
    Write-Error "Main source not found: $mainSource"
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    exit 1
}

$runtimeSources = @($cFiles | Where-Object { $_ -ne $mainSource })
if ($runtimeSources.Count -eq 0) {
    Write-Error "No runtime sources found after excluding '$mainSource'"
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
    $runtimeArgs = @(
        "/std:c17", "/Gd", "/O2", "/Gy", "/GF", "/GL", "/W4", "/WX", "/MP", "/nologo",
        "/LD", "/I$src",
        "/Fe:$runtimeDllName"
    )
    $runtimeArgs += $runtimeSources
    $runtimeArgs += @(
        "/link",
        "/DEF:$runtimeDef",
        "/IMPLIB:$runtimeLibName"
    )

    Write-Host "Invoking: cl.exe $($runtimeArgs -join ' ')"
    & cl.exe @runtimeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "cl.exe returned exit code $LASTEXITCODE while building shared runtime"
    }

    $runtimeDllPath = Join-Path $buildDir $runtimeDllName
    $runtimeLibPath = Join-Path $buildDir $runtimeLibName
    $runtimePdbPath = Join-Path $buildDir $runtimePdbName

    if (-not (Test-Path $runtimeDllPath)) {
        throw "Expected runtime DLL not found: $runtimeDllPath"
    }
    if (-not (Test-Path $runtimeLibPath)) {
        throw "Expected runtime import library not found: $runtimeLibPath"
    }

    Copy-Item -Path $runtimeDllPath -Destination $runtimeDllDest -Force
    Copy-Item -Path $runtimeLibPath -Destination $runtimeLibDest -Force
    if (Test-Path $runtimePdbPath) {
        Copy-Item -Path $runtimePdbPath -Destination $runtimePdbDest -Force
    }
    Write-Host "Copied runtime DLL to: $runtimeDllDest"
    Write-Host "Copied runtime import library to: $runtimeLibDest"

    $exeArgs = @(
        "/std:c17", "/Gd", "/O2", "/Gy", "/GF", "/GL", "/W4", "/WX", "/MP", "/nologo",
        "/I$src",
        "/Fe:prefix.exe",
        $mainSource,
        $runtimeLibPath
    )

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
        Write-Host "No extension C sources found under ext/, lib/"
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
                "/std:c17", "/Gd", "/O2", "/W4", "/WX", "/nologo", "/LD", "/LTCG",
                "/I$src",
                "/Fe:$extOutName",
                $extSourcePath,
                $runtimeLibPath
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

Write-Host "Build succeeded and artifacts copied to: $(Join-Path $script 'prefix.exe'), $runtimeDllDest, $runtimeLibDest"
exit 0
