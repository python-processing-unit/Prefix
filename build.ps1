<#
build.ps1
Compiles the Prefix runtime into a shared library, links the interpreter
against that library, and compiles each discovered extension against the
same shared runtime.

Requires: clang on PATH targeting x64.
Usage (from Prefix folder):
    pwsh -File ./build.ps1
#>

$script = Split-Path -Parent $MyInvocation.MyCommand.Definition
$src = Join-Path $script "src"
$runtimeDef = Join-Path $src "prefix_runtime.def"
$extRoots = @(
    (Join-Path $script "ext"),
    (Join-Path $script "lib"),
    (Join-Path $script "tests")
)

$runningOnWindows = -not $IsLinux -and -not $IsMacOS

# Platform-specific names
if ($runningOnWindows) {
    $clangCmd = "clang.exe"
    $runtimeDllName = "prefix_runtime.dll"
    $runtimeLibName = "prefix_runtime.lib"
    $runtimePdbName = "prefix_runtime.pdb"
    $exeName = "prefix.exe"
} elseif ($IsMacOS) {
    $clangCmd = "clang"
    $runtimeDllName = "libprefix_runtime.dylib"
    $runtimeLibName = ""
    $runtimePdbName = ""
    $exeName = "prefix"
} else {
    $clangCmd = "clang"
    $runtimeDllName = "libprefix_runtime.so"
    $runtimeLibName = ""
    $runtimePdbName = ""
    $exeName = "prefix"
}

$marchArg = "-march=x86-64"
if (-not $runningOnWindows) {
    $marchArg = $null
}

$runtimeDllDest = Join-Path $script $runtimeDllName
$runtimeLibDest = if ($runtimeLibName) { Join-Path $script $runtimeLibName } else { $null }
$runtimePdbDest = if ($runtimePdbName) { Join-Path $script $runtimePdbName } else { $null }

$extSuffix = ".dll"
if ($IsLinux) {
    $extSuffix = ".so"
} elseif ($IsMacOS) {
    $extSuffix = ".dylib"
}

Write-Host "Preparing temp build directory..."
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
if ($runningOnWindows) {
    $buildDir = Join-Path $env:TEMP ("prefix-build-$stamp")
} else {
    $buildDir = Join-Path "/tmp" ("prefix-build-$stamp")
}
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
Write-Host "Build dir: $buildDir"

$clang = Get-Command $clangCmd -ErrorAction SilentlyContinue
if (-not $clang) {
    Write-Error "$clangCmd not found on PATH."
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    exit 1
}

$clangTarget = (& $clangCmd -dumpmachine 2>$null | Select-Object -First 1)
if (-not $clangTarget) {
    Write-Error "Unable to determine $clangCmd target triple."
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    exit 1
}

if ($runningOnWindows -and $clangTarget -notmatch '^x86_64-') {
    Write-Error "$clangCmd must target baseline x64. Found target '$clangTarget'."
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    exit 1
}

$cFiles = Get-ChildItem -Path $src -Filter *.c -File -Recurse | ForEach-Object { $_.FullName }
if ($cFiles.Count -eq 0) {
    Write-Error "No .c files found in '$src'"
    Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    exit 1
}

if ($runningOnWindows -and -not (Test-Path $runtimeDef)) {
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

Push-Location $buildDir
try {
    if ($runningOnWindows) {
        # MSVC-compatible driver mode for Windows
        $clangArgs = @(
            "--driver-mode=cl",
            "/clang:$marchArg",
            "/clang:-fuse-ld=lld",
            "/clang:-flto=full",
            "/clang:-ffunction-sections",
            "/clang:-fdata-sections"
        )
        $releaseCompileArgs = @(
            "/std:c17", "/Gd", "/O2", "/Ot", "/Oi", "/Ob2", "/Gy", "/Gw", "/GF", "/W4", "/WX", "/nologo"
        )
        $linkLibs = @("ole32.lib", "ws2_32.lib", "winhttp.lib", "user32.lib", "gdi32.lib")
        $clangArgs += "/clang:-Wno-deprecated-declarations"
    } else {
        # GCC-compatible flags for Linux/macOS
        $clangArgs = @(
            "-flto=full",
            "-ffunction-sections",
            "-fdata-sections",
            "-fPIC"
        )
        if ($marchArg) {
            $clangArgs = @($marchArg) + $clangArgs
        }
        $releaseCompileArgs = @(
            "-std=c17", "-O2", "-Wall", "-Wextra", "-Werror",
            "-D_GNU_SOURCE"
        )
        $linkLibs = @()
        if ($IsLinux) {
            $linkLibs = @("-ldl", "-lpthread", "-lm")
        } elseif ($IsMacOS) {
            $linkLibs = @("-ldl", "-lm")
        }
    }

    # --- Build runtime shared library ---
    if ($runningOnWindows) {
        $runtimeArgs = @($clangArgs + $releaseCompileArgs + @(
            "/LD", "/I$src",
            "/Fe:$runtimeDllName"
        ))
        $runtimeArgs += $runtimeSources
        $runtimeLinkFlags = @("/link", "/DEF:$runtimeDef", "/IMPLIB:$runtimeLibName", "/OPT:REF", "/OPT:ICF")
        $runtimeArgs += $runtimeLinkFlags
    } else {
        $runtimeArgs = @($clangArgs + $releaseCompileArgs + @(
            "-shared",
            "-I$src",
            "-o", $runtimeDllName
        ))
        $runtimeArgs += $runtimeSources
        $runtimeArgs += @("-Wl,--gc-sections")
        $runtimeArgs += $linkLibs
    }

    Write-Host "Invoking: $clangCmd $($runtimeArgs -join ' ')"
    & $clangCmd @runtimeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "$clangCmd returned exit code $LASTEXITCODE while building shared runtime"
    }

    $runtimeDllPath = Join-Path $buildDir $runtimeDllName

    if (-not (Test-Path $runtimeDllPath)) {
        throw "Expected runtime library not found: $runtimeDllPath"
    }

    Copy-Item -Path $runtimeDllPath -Destination $runtimeDllDest -Force
    Write-Host "Copied runtime library to: $runtimeDllDest"

    if ($runningOnWindows) {
        $runtimeLibPath = Join-Path $buildDir $runtimeLibName
        $runtimePdbPath = Join-Path $buildDir $runtimePdbName
        if (-not (Test-Path $runtimeLibPath)) {
            throw "Expected runtime import library not found: $runtimeLibPath"
        }
        Copy-Item -Path $runtimeLibPath -Destination $runtimeLibDest -Force
        if (Test-Path $runtimePdbPath) {
            Copy-Item -Path $runtimePdbPath -Destination $runtimePdbDest -Force
        }
        Write-Host "Copied runtime import library to: $runtimeLibDest"
    }

    # --- Build interpreter executable ---
    if ($runningOnWindows) {
        $runtimeLibPath = Join-Path $buildDir $runtimeLibName
        $exeArgs = @($clangArgs + $releaseCompileArgs + @(
            "/I$src",
            "/Fe:$exeName",
            $mainSource,
            $runtimeLibPath
        ))
        $exeLinkFlags = @("/link", "/OPT:REF", "/OPT:ICF")
        $exeArgs += $exeLinkFlags
    } else {
        $exeArgs = @($clangArgs + $releaseCompileArgs + @(
            "-I$src",
            "-o", $exeName,
            $mainSource,
            "-L$buildDir",
            "-lprefix_runtime",
            "-Wl,-rpath,`$ORIGIN",
            "-Wl,--gc-sections"
        ))
        $exeArgs += $linkLibs
    }

    Write-Host "Invoking: $clangCmd $($exeArgs -join ' ')"
    & $clangCmd @exeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "$clangCmd returned exit code $LASTEXITCODE while building interpreter"
    }

    $outExe = Join-Path $buildDir $exeName
    if (-not (Test-Path $outExe)) {
        throw "Expected output executable not found: $outExe"
    }

    $exeDest = Join-Path $script $exeName
    Copy-Item -Path $outExe -Destination $exeDest -Force
    Write-Host "Copied executable to: $exeDest"

    # --- Build extensions ---
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
        $extBuildDir = Join-Path $buildDir "ext-$extName"

        New-Item -ItemType Directory -Path $extBuildDir -Force | Out-Null
        Push-Location $extBuildDir
        try {
            if ($runningOnWindows) {
                $extArgs = @($clangArgs + $releaseCompileArgs + @(
                    "/LD",
                    "/I$src",
                    "/Fe:$extOutName",
                    $extSourcePath
                ))
                if ($linkLibs.Count -gt 0) { $extArgs += $linkLibs }
                $extArgs += (Join-Path $buildDir $runtimeLibName)
                $extArgs += @("/link", "/OPT:REF", "/OPT:ICF")
            } else {
                $extArgs = @($clangArgs + $releaseCompileArgs + @(
                    "-shared",
                    "-I$src",
                    "-o", $extOutName,
                    $extSourcePath,
                    "-L$buildDir",
                    "-lprefix_runtime",
                    "-Wl,--gc-sections"
                ))
                if ($linkLibs.Count -gt 0) { $extArgs += $linkLibs }
            }

            Write-Host "Invoking: $clangCmd $($extArgs -join ' ')"
            & $clangCmd @extArgs
            if ($LASTEXITCODE -ne 0) {
                throw "$clangCmd returned exit code $LASTEXITCODE while building extension '$extSourcePath'"
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

Write-Host "Build succeeded and artifacts copied to: $(Join-Path $script $exeName), $runtimeDllDest"
exit 0
