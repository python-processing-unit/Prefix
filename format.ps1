<#
format.ps1
Formats and lint-fixes the Prefix C codebase with Clang tools.

Usage (from Prefix folder):
    powershell -ExecutionPolicy Bypass -File .\format.ps1
#>

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$srcDir = Join-Path $scriptDir "src"

if (-not (Test-Path $srcDir)) {
    Write-Error "Could not locate src directory at: $srcDir"
    exit 1
}

$clang = Get-Command clang.exe -ErrorAction SilentlyContinue
$clangFormat = Get-Command clang-format.exe -ErrorAction SilentlyContinue
$clangTidy = Get-Command clang-tidy.exe -ErrorAction SilentlyContinue

if (-not $clang) {
    Write-Error "clang.exe not found on PATH."
    exit 1
}
if (-not $clangFormat) {
    Write-Error "clang-format.exe not found on PATH."
    exit 1
}
if (-not $clangTidy) {
    Write-Error "clang-tidy.exe not found on PATH."
    exit 1
}

# clang-tidy checks: fix-oriented defaults that stay quiet unless they can rewrite code.
$clangTidyChecks = "readability-braces-around-statements,readability-isolate-declaration,readability-redundant-control-flow"
$clangFormatStyle = "{BasedOnStyle: LLVM, IndentWidth: 4, UseTab: Never, ColumnLimit: 120}"
$clangTidyFormatArg = "-format-style=$clangFormatStyle"
$roots = @(
    (Join-Path $scriptDir "src"),
    (Join-Path $scriptDir "ext"),
    (Join-Path $scriptDir "lib"),
    (Join-Path $scriptDir "tests\helpers")
)

$allC = @()
$allH = @()

foreach ($root in $roots) {
    if (-not (Test-Path $root)) {
        continue
    }
    $allC += Get-ChildItem -Path $root -Recurse -File -Filter *.c | ForEach-Object { $_.FullName }
    $allH += Get-ChildItem -Path $root -Recurse -File -Filter *.h | ForEach-Object { $_.FullName }
}

$sourceFiles = @($allC | Sort-Object -Unique)
$headerFiles = @($allH | Sort-Object -Unique)
$formatFiles = @($sourceFiles + $headerFiles | Sort-Object -Unique)

if ($sourceFiles.Count -eq 0) {
    Write-Error "No C source files found under src/, ext/, lib/, or tests/helpers/."
    exit 1
}

if ($formatFiles.Count -eq 0) {
    Write-Error "No C/C header files found to format."
    exit 1
}

Write-Host "Formatting $($formatFiles.Count) C/C header file(s) with clang-format (IndentWidth=$($clangFormatStyle -replace '^\{|\}$',''))..."
foreach ($file in $formatFiles) {
    & $clangFormat.Path "-style=$clangFormatStyle" -i $file
    if ($LASTEXITCODE -ne 0) {
        Write-Error "clang-format failed for: $file"
        exit 1
    }
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$tidyDbDir = Join-Path $env:TEMP ("prefix-clang-tidy-" + $stamp)
New-Item -ItemType Directory -Path $tidyDbDir -Force | Out-Null

try {
    $compileDbPath = Join-Path $tidyDbDir "compile_commands.json"
    $compileDb = @()

    foreach ($file in $sourceFiles) {
        $command = ('"{0}" --driver-mode=cl /std:c17 /I"{1}" /I"{2}" /D_CRT_SECURE_NO_WARNINGS "{3}"' -f $clang.Path, $srcDir, $scriptDir, $file)
        $compileDb += [ordered]@{
            directory = $scriptDir
            file = $file
            command = $command
        }
    }

    $compileDb | ConvertTo-Json -Depth 4 | Set-Content -Path $compileDbPath -Encoding UTF8

    Write-Host "Lint-fixing $($sourceFiles.Count) C source file(s) with clang-tidy..."
    $lintFixFailures = @()
    foreach ($file in $sourceFiles) {
        $tidyArgs = @('-p', $tidyDbDir)
        if ($clangTidyChecks -ne "") { $tidyArgs += "--checks=$clangTidyChecks" }
        # Narrow header filter to just project files (src/, ext/, lib/, tests/) to avoid analyzing system headers
        $headerFilter = "(src|ext|lib|tests)/"
        $tidyArgs += @('-quiet', '-fix', '-fix-errors', '-fix-notes', $clangTidyFormatArg, "-header-filter=$headerFilter", $file)
        $tidyOutput = & $clangTidy.Path @tidyArgs 2>&1

        if ($LASTEXITCODE -ne 0) {
            $lintFixFailures += [pscustomobject]@{
                File = $file
                Output = (($tidyOutput | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine)
            }
        }
    }

    if ($lintFixFailures.Count -gt 0) {
        Write-Host "clang-tidy auto-fix failed for $($lintFixFailures.Count) file(s):" -ForegroundColor Red
        foreach ($failure in $lintFixFailures) {
            Write-Host "--- $($failure.File) ---" -ForegroundColor Red
            if ($failure.Output) {
                Write-Host $failure.Output -ForegroundColor Red
            }
        }
        Write-Error "clang-tidy auto-fix failed."
        exit 1
    }

    Write-Host "Re-formatting after clang-tidy fixes..."
    foreach ($file in $formatFiles) {
        & $clangFormat.Path "-style=$clangFormatStyle" -i $file
        if ($LASTEXITCODE -ne 0) {
            Write-Error "clang-format post-lint failed for: $file"
            exit 1
        }
    }

    Write-Host "Formatting and lint auto-fix completed successfully." -ForegroundColor Green
    exit 0
} finally {
    if (Test-Path $tidyDbDir) {
        Remove-Item -Recurse -Force $tidyDbDir -ErrorAction SilentlyContinue
    }
}