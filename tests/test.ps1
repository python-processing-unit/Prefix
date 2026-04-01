$testDir = $PSScriptRoot
$prefixDir = Split-Path -Parent $testDir
$casesDir = Join-Path $testDir 'cases'
$exePath = Join-Path $prefixDir 'prefix.exe'

function Assert([bool]$Condition, [string]$Message = 'Assertion failed') {
  if (-not $Condition) {
    throw $Message
  }
}

function Invoke-TestCase([string]$RelativePath) {
  $casePath = Join-Path $casesDir $RelativePath
  Assert (Test-Path $casePath) "Test case not found: $RelativePath"

  $extension = [IO.Path]::GetExtension($casePath)
  $output = @()

  if ($extension -ieq '.ps1') {
    $powerShellExe = if ($PSVersionTable.PSEdition -eq 'Core') {
      Join-Path $PSHOME 'pwsh.exe'
    } else {
      Join-Path $PSHOME 'powershell.exe'
    }
    Assert (Test-Path $powerShellExe) "PowerShell executable not found at: $powerShellExe"

    $powerShellCommand = "& { `$ErrorActionPreference = 'Stop'; & `$args[0] }"
    $output = & $powerShellExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command $powerShellCommand $casePath 2>&1
  } else {
    $output = & $exePath $casePath 2>&1
  }

  $exitCode = [int]$LASTEXITCODE

  return [pscustomobject]@{
    ExitCode = $exitCode
    Output = (($output | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine)
  }
}

$passingDir = Join-Path $casesDir 'passing'
$passingCases = @()
if (Test-Path $passingDir) {
  $passingCases = Get-ChildItem -Path $passingDir -File | Where-Object { $_.Extension -in '.pre', '.ps1' } | Sort-Object Name | ForEach-Object { Join-Path 'passing' $_.Name }
} else {
  Write-Host "Warning: passing cases directory not found: $passingDir"
}

$failingDir = Join-Path $casesDir 'failing'
$failingCases = @()
if (Test-Path $failingDir) {
  $failingCases = Get-ChildItem -Path $failingDir -File | Where-Object { $_.Extension -in '.pre', '.ps1' } | Sort-Object Name | ForEach-Object { Join-Path 'failing' $_.Name }
} else {
  Write-Host "Warning: failing cases directory not found: $failingDir"
}

Assert (Test-Path $exePath) "Interpreter executable not found at: $exePath"
Assert (Test-Path $casesDir) "Test cases directory not found at: $casesDir"

Write-Host 'Running tests...'

$failures = @()
$total = 0

foreach ($case in $passingCases) {
  $total += 1
  $result = Invoke-TestCase $case
  Write-Host "==> $($case): " -NoNewline
  if ($result.ExitCode -ne 0) {
    Write-Host 'fail' -ForegroundColor Red
    Write-Host $result.Output -ForegroundColor Red
    $failures += [pscustomobject]@{
      Path = $case
      Type = 'UnexpectedFailure'
      ExitCode = $result.ExitCode
      Output = $result.Output
    }
  } else {
    Write-Host 'pass' -ForegroundColor Green
  }
}

foreach ($case in $failingCases) {
  $total += 1
  $result = Invoke-TestCase $case
  Write-Host "==> $($case): " -NoNewline
  if ($result.ExitCode -eq 0) {
    Write-Host 'fail' -ForegroundColor Red
    $failures += [pscustomobject]@{
      Path = $case
      Type = 'UnexpectedSuccess'
      ExitCode = $result.ExitCode
      Output = $result.Output
    }
  } else {
    Write-Host 'pass' -ForegroundColor Green
  }
}

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host 'FAILURES:' -ForegroundColor Red
  foreach ($f in $failures) {
    Write-Host " - $($f.Path): $($f.Type) (ExitCode: $($f.ExitCode))"
    if ($f.Output) { Write-Host $f.Output -ForegroundColor Red}
  }
  Write-Host ''
  Write-Host "Completed with $($total - $failures.Count)/$($total) tests passing, $($failures.Count)/$($total) tests failing." -ForegroundColor Red
  exit 1
} else {
  Write-Host "Completed with $($total)/$($total) tests passing, 0/$($total) tests failing." -ForegroundColor Green
  exit 0
}
