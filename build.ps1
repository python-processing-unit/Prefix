<#
build.ps1
Compiles the Prefix runtime into a shared DLL, links the interpreter EXE
against that DLL's import library, and compiles each discovered extension
against the same shared runtime.

Requires: clang.exe on PATH targeting x64.
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

$clang = Get-Command clang.exe -ErrorAction SilentlyContinue
if (-not $clang) {
    Write-Error "clang.exe not found on PATH."
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    exit 1
}

$clangTarget = (& clang.exe -dumpmachine 2>$null | Select-Object -First 1)
if (-not $clangTarget) {
    Write-Error "Unable to determine clang.exe target triple."
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    exit 1
}

if ($clangTarget -notmatch '^x86_64-') {
    Write-Error "clang.exe must target baseline x64. Found target '$clangTarget'."
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
    $runningOnWindows = $platform::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)

    # Use a single release profile for every artifact: portable baseline x64,
    # full-program optimization, and link-time dead stripping/folding.
    $clangArgs = @(
        "--driver-mode=cl",
        "/clang:-march=x86-64",
        "/clang:-fuse-ld=lld",
        "/clang:-flto=full",
        "/clang:-ffunction-sections",
        "/clang:-fdata-sections"
    )
    $releaseCompileArgs = @(
        "/std:c17", "/Gd", "/O2", "/Ot", "/Oi", "/Ob2", "/Gy", "/Gw", "/GF", "/W4", "/WX", "/nologo"
    )
    # Only pass the `--gc-sections` linker option on non-Windows platforms;
    # on Windows we use MSVC-style linker options (/OPT:REF /OPT:ICF).
    if (-not $runningOnWindows) {
        $clangArgs += "/clang:-Wl,--gc-sections"
    }

    # Libraries that extensions may need on Windows. Link via the build
    # system instead of source-level linker pragmas in the extension code.
    $linkLibs = @()
    if ($runningOnWindows) {
        $linkLibs = @("ole32.lib", "ws2_32.lib", "winhttp.lib", "user32.lib", "gdi32.lib")
        $clangArgs += "/clang:-Wno-deprecated-declarations"
    }

    $runtimeArgs = @($clangArgs + $releaseCompileArgs + @(
        "/LD", "/I$src",
        "/Fe:$runtimeDllName"
    ))
    $runtimeArgs += $runtimeSources
    $runtimeLinkFlags = @("/link", "/DEF:$runtimeDef", "/IMPLIB:$runtimeLibName")
    if ($runningOnWindows) {
        $runtimeLinkFlags += "/OPT:REF"
        $runtimeLinkFlags += "/OPT:ICF"
    } else {
        $runtimeLinkFlags += "/clang:-Wl,--gc-sections"
    }
    $runtimeArgs += $runtimeLinkFlags

    Write-Host "Invoking: clang.exe $($runtimeArgs -join ' ')"
    & clang.exe @runtimeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "clang.exe returned exit code $LASTEXITCODE while building shared runtime"
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

    $exeArgs = @($clangArgs + $releaseCompileArgs + @(
        "/I$src",
        "/Fe:prefix.exe",
        $mainSource,
        $runtimeLibPath
    ))
    $exeLinkFlags = @()
    if ($runningOnWindows) {
        $exeLinkFlags += "/link"
        $exeLinkFlags += "/OPT:REF"
        $exeLinkFlags += "/OPT:ICF"
    } else {
        $exeLinkFlags += "/clang:-Wl,--gc-sections"
    }
    $exeArgs += $exeLinkFlags

    Write-Host "Invoking: clang.exe $($exeArgs -join ' ')"
    & clang.exe @exeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "clang.exe returned exit code $LASTEXITCODE while building interpreter"
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
            $extArgs = @($clangArgs + $releaseCompileArgs + @(
                "/LD",
                "/I$src",
                "/Fe:$extOutName",
                $extSourcePath
            ))
            # Add OS-specific libraries required by some extensions.
            if ($linkLibs.Count -gt 0) { $extArgs += $linkLibs }
            $extArgs += $runtimeLibPath
            $extLinkFlags = @()
            if ($runningOnWindows) {
                $extLinkFlags += "/link"
                $extLinkFlags += "/OPT:REF"
                $extLinkFlags += "/OPT:ICF"
            } else {
                $extLinkFlags += "/clang:-Wl,--gc-sections"
            }
            $extArgs += $extLinkFlags

            Write-Host "Invoking: clang.exe $($extArgs -join ' ')"
            & clang.exe @extArgs
            if ($LASTEXITCODE -ne 0) {
                throw "clang.exe returned exit code $LASTEXITCODE while building extension '$extSourcePath'"
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
