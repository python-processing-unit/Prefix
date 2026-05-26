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

# Record the wall-clock time when the first test case is invoked.
$testStart = Get-Date

# Combine passing and failing cases and sort them alphabetically.
$allCases = @()
foreach ($case in $passingCases) { $allCases += [pscustomobject]@{ RelativePath = $case; ExpectedPass = $true } }
foreach ($case in $failingCases) { $allCases += [pscustomobject]@{ RelativePath = $case; ExpectedPass = $false } }
$allCases = $allCases | Sort-Object RelativePath

# Mark tests that touch shared prefix roots (lib/ext) as exclusive so they
# run with exclusive access and do not race with other tests.
$casesWithFlags = @()
foreach ($c in $allCases) {
  $casePath = Join-Path $casesDir $c.RelativePath
  $content = ''
  try { $content = Get-Content -Path $casePath -Raw -ErrorAction Stop } catch { $content = '' }

  # If the test references lib\std, lib\usr, ext\std or ext\usr (or
  # explicitly stages probes), treat it as exclusive to avoid races.
  $isExclusive = $false
  if ($content -match 'lib\\std|lib\\usr|ext\\std|ext\\usr|Add-StagedProbe|Copy-Item -Path') {
    $isExclusive = $true
  }

  $casesWithFlags += [pscustomobject]@{ RelativePath = $c.RelativePath; ExpectedPass = $c.ExpectedPass; Exclusive = $isExclusive }
}

$allCases = $casesWithFlags

$total = $allCases.Count

# If there are no tests, behave as before and exit cleanly.
if ($total -eq 0) {
  $testEnd = Get-Date
  $elapsed = $testEnd - $testStart
  $elapsedSeconds = '{0:N3}' -f $elapsed.TotalSeconds
  Write-Host "Ran 0 tests in $elapsedSeconds seconds, with 0/0 tests passing, 0/0 tests failing." -ForegroundColor Green
  exit 0
}

# Determine number of worker threads: logical processors - 1, or 1 minimum.
$maxThreads = [Math]::Max(1, [Environment]::ProcessorCount - 1)

# Prepare concurrent queues for tasks and results.
$taskQueue = [System.Collections.Concurrent.ConcurrentQueue[object]]::new()
$resultsQueue = [System.Collections.Concurrent.ConcurrentQueue[object]]::new()
foreach ($c in $allCases) { $taskQueue.Enqueue($c) }

# Determine PowerShell executable path for running .ps1 cases.
$powerShellExe = if ($PSVersionTable.PSEdition -eq 'Core') { Join-Path $PSHOME 'pwsh.exe' } else { Join-Path $PSHOME 'powershell.exe' }

# Create a runspace pool and start worker runspaces.

# Reader/writer lock used so "exclusive" tests run alone and non-exclusive
# tests may run concurrently.
$rwLock = New-Object System.Threading.ReaderWriterLockSlim

$runspacePool = [runspacefactory]::CreateRunspacePool(1, $maxThreads)
$runspacePool.Open()

$workerScript = @'
param($queue, $exePath, $casesDir, $powerShellExe, $resultsQueue, $rwLock)

while ($true) {
    $item = $null
    if (-not $queue.TryDequeue([ref]$item)) { break }

    $casePath = Join-Path $casesDir $item.RelativePath
    if (-not (Test-Path $casePath)) {
        $resultsQueue.Enqueue([pscustomobject]@{ Path = $item.RelativePath; ExitCode = -1; Output = "Test case not found: $($item.RelativePath)"; ExpectedPass = $item.ExpectedPass })
        continue
    }

    $extension = [IO.Path]::GetExtension($casePath)

    # Acquire a read lock for normal tests, a write lock for exclusive tests.
    if ($item.Exclusive) {
      $rwLock.EnterWriteLock()
    } else {
      $rwLock.EnterReadLock()
    }

    try {
      $output = @()
      if ($extension -ieq '.ps1') {
        if (-not (Test-Path $powerShellExe)) { throw "PowerShell executable not found at: $powerShellExe" }
        $powerShellCommand = "& { `$ErrorActionPreference = 'Stop'; & `$args[0] }"
        $output = & $powerShellExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command $powerShellCommand $casePath 2>&1
      } else {
        $output = & $exePath $casePath 2>&1
      }

      $exitCode = [int]$LASTEXITCODE
      $resultsQueue.Enqueue([pscustomobject]@{
        Path = $item.RelativePath
        ExitCode = $exitCode
        Output = (($output | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine)
        ExpectedPass = $item.ExpectedPass
      })
    }
    finally {
      if ($item.Exclusive) {
        $rwLock.ExitWriteLock()
      } else {
        $rwLock.ExitReadLock()
      }
    }
}
'@

$workers = @()
for ($i = 0; $i -lt $maxThreads; $i++) {
    $ps = [PowerShell]::Create()
    $ps.RunspacePool = $runspacePool
    $ps.AddScript($workerScript) | Out-Null
  [void]$ps.AddArgument($taskQueue)
  [void]$ps.AddArgument($exePath)
  [void]$ps.AddArgument($casesDir)
  [void]$ps.AddArgument($powerShellExe)
  [void]$ps.AddArgument($resultsQueue)
  [void]$ps.AddArgument($rwLock)
  $async = $ps.BeginInvoke()
  $workers += [pscustomobject]@{ PowerShell = $ps; Async = $async }
}

# Show a single-line progress indicator that updates on each completed test.
$lastDisplayedLength = 0
while ($true) {
    $completed = $resultsQueue.Count
    if ($completed -gt 0 -or $completed -eq 0) {
        $remaining = $total - $completed
        $message = "${completed}/${total} test completed, ${remaining} remaining."
        $pad = if ($message.Length -lt $lastDisplayedLength) { ' ' * ($lastDisplayedLength - $message.Length) } else { '' }
        $lastDisplayedLength = $message.Length
        Write-Host -NoNewline "`r$message$pad"
    }
    if ($completed -ge $total) { break }
    Start-Sleep -Milliseconds 100
}

# Ensure a newline after the single-line progress.
Write-Host ''

# Wait for workers to finish and dispose runspaces.
foreach ($w in $workers) {
    try { $w.PowerShell.EndInvoke($w.Async) } catch { }
    $w.PowerShell.Dispose()
}
$runspacePool.Close()
$runspacePool.Dispose()

# Collect results and determine failures.
$failures = @()
$results = @()
$res = $null
while ($resultsQueue.TryDequeue([ref]$res)) { $results += $res }

foreach ($r in $results) {
    if ($r.ExpectedPass -and $r.ExitCode -ne 0) {
        $failures += [pscustomobject]@{
            Path = $r.Path
            Type = 'UnexpectedFailure'
            ExitCode = $r.ExitCode
            Output = $r.Output
        }
    } elseif (-not $r.ExpectedPass -and $r.ExitCode -eq 0) {
        $failures += [pscustomobject]@{
            Path = $r.Path
            Type = 'UnexpectedSuccess'
            ExitCode = $r.ExitCode
            Output = $r.Output
        }
    }
}

$testEnd = Get-Date
$elapsed = $testEnd - $testStart
$elapsedSeconds = '{0:N3}' -f $elapsed.TotalSeconds

if ($failures.Count -gt 0) {
  Write-Host 'FAILURES:' -ForegroundColor Red
  foreach ($f in $failures) {
    Write-Host " - $($f.Path): $($f.Type) (ExitCode: $($f.ExitCode))"
    if ($f.Output) { Write-Host $f.Output -ForegroundColor Red }
  }
  Write-Host ''
  Write-Host "Ran $total tests in $elapsedSeconds seconds, with $($total - $failures.Count)/$($total) tests passing, $($failures.Count)/$($total) tests failing." -ForegroundColor Red
  exit 1
} else {
  Write-Host "Ran $total tests in $elapsedSeconds seconds, with $($total)/$($total) tests passing, 0/$($total) tests failing." -ForegroundColor Green
  exit 0
}
