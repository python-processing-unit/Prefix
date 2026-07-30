$testDir = $PSScriptRoot
$prefixDir = Split-Path -Parent $testDir
$casesDir = Join-Path $testDir 'cases'

$exeName = if ($IsLinux -or $IsMacOS) { 'prefix' } else { 'prefix.exe' }
$exePath = Join-Path $prefixDir $exeName

function Assert([bool]$Condition, [string]$Message = 'Assertion failed') {
  if (-not $Condition) {
    throw $Message
  }
}

$yamlFiles = @()
if (Test-Path $casesDir) {
  $yamlFiles = Get-ChildItem -Path $casesDir -Recurse -File -Filter '*.yaml' | Sort-Object FullName | ForEach-Object { $_.FullName }
} else {
  Write-Host "Warning: test cases directory not found: $casesDir"
}

$testStart = Get-Date

$script:lineIndex = 0

function ConvertFrom-YamlFile {
  param([string]$Path)
  $text = Get-Content -Path $Path -Raw -Encoding UTF8
  $lines = $text -split "`r?`n"

  $result = @{}
  $script:lineIndex = 0

  while ($script:lineIndex -lt $lines.Count) {
    $line = $lines[$script:lineIndex]
    $trimmed = $line.TrimStart()
    $indent = $line.Length - $trimmed.Length

    if ($trimmed -match '^passing:\s*(.*)') {
      $script:lineIndex++
      $result.passing = ConvertFrom-YamlSequence $lines $indent
    } elseif ($trimmed -match '^failing:\s*(.*)') {
      $script:lineIndex++
      $result.failing = ConvertFrom-YamlSequence $lines $indent
    } else {
      $script:lineIndex++
    }
  }

  return $result
}

function ConvertFrom-YamlSequence {
  param($lines, $keyIndent)
  $items = @()
  $baseIndent = $keyIndent + 2

  while ($script:lineIndex -lt $lines.Count) {
    $line = $lines[$script:lineIndex]
    $trimmed = $line.TrimStart()
    $indent = $line.Length - $trimmed.Length

    if ($trimmed -eq '') {
      $script:lineIndex++
      continue
    }
    if ($indent -lt $baseIndent) {
      break
    }
    if ($indent -eq $baseIndent -and $trimmed -match '^- ') {
      $mapping = ConvertFrom-YamlMapping $lines $indent
      $items += $mapping
    } elseif ($indent -gt $baseIndent) {
      $script:lineIndex++
    } else {
      $script:lineIndex++
    }
  }
  return $items
}

function ConvertFrom-YamlMapping {
  param($lines, $dashIndent)
  $item = @{}
  $baseIndent = $dashIndent + 2

  while ($script:lineIndex -lt $lines.Count) {
    $line = $lines[$script:lineIndex]
    $trimmed = $line.TrimStart()
    $indent = $line.Length - $trimmed.Length

    if ($trimmed -eq '' -or $indent -lt $dashIndent) {
      break
    }
    if ($indent -eq $dashIndent -and $trimmed -match '^- ([a-zA-Z_][\w-]*)\s*:\s*(.*)') {
      if ($item.Count -gt 0) {
        break
      }
      $key = $matches[1]
      $valuePart = $matches[2]
      $script:lineIndex++

      if ($valuePart -match '^\|') {
        $item[$key] = ConvertFrom-YamlBlockScalar $lines $dashIndent
      } elseif ($valuePart -match '^"(.*)"$') {
        $item[$key] = $matches[1]
      } elseif ($valuePart -match "'(.*)'") {
        $item[$key] = $matches[1]
      } elseif ($valuePart -match '^(true|false)$') {
        $item[$key] = [bool]::Parse($valuePart)
      } elseif ($valuePart -match '^-?\d+$') {
        $item[$key] = [int]$valuePart
      } elseif ($valuePart -match '^-?\d+\.\d+$') {
        $item[$key] = [double]$valuePart
      } elseif ($valuePart -eq '') {
        $item[$key] = ''
      } else {
        $item[$key] = $valuePart
      }
    } elseif ($indent -eq $baseIndent -and $trimmed -match '^([a-zA-Z_][\w-]*)\s*:\s*(.*)') {
      $key = $matches[1]
      $valuePart = $matches[2]

      if ($valuePart -match '^\|') {
        $item[$key] = ConvertFrom-YamlBlockScalar $lines $baseIndent
      } elseif ($valuePart -match '^"(.*)"$') {
        $item[$key] = $matches[1]
        $script:lineIndex++
      } elseif ($valuePart -match "'(.*)'") {
        $item[$key] = $matches[1]
        $script:lineIndex++
      } elseif ($valuePart -match '^(true|false)$') {
        $item[$key] = [bool]::Parse($valuePart)
        $script:lineIndex++
      } elseif ($valuePart -match '^-?\d+$') {
        $item[$key] = [int]$valuePart
        $script:lineIndex++
      } elseif ($valuePart -match '^-?\d+\.\d+$') {
        $item[$key] = [double]$valuePart
        $script:lineIndex++
      } elseif ($valuePart -eq '') {
        $item[$key] = ''
        $script:lineIndex++
      } else {
        $item[$key] = $valuePart
        $script:lineIndex++
      }
    } elseif ($indent -eq $dashIndent -and $trimmed -match '^- ') {
      break
    } else {
      $script:lineIndex++
    }
  }
  return [pscustomobject]$item
}

function ConvertFrom-YamlBlockScalar {
  param($lines, $keyIndent)
  $linesList = @()
  $script:lineIndex++
  $contentIndent = $null

  for ($j = $script:lineIndex; $j -lt $lines.Count; $j++) {
    $line = $lines[$j]
    $trimmed = $line.TrimStart()
    if ($trimmed -ne '') {
      $contentIndent = $line.Length - $trimmed.Length
      break
    }
  }

  if ($null -eq $contentIndent -or $contentIndent -le $keyIndent) {
    return ''
  }

  while ($script:lineIndex -lt $lines.Count) {
    $line = $lines[$script:lineIndex]
    $trimmed = $line.TrimStart()
    $indent = $line.Length - $trimmed.Length

    if ($trimmed -ne '' -and $indent -le $keyIndent) {
      break
    }
    if ($trimmed -ne '' -and $indent -lt $contentIndent) {
      break
    }
    if ($trimmed -ne '') {
      $linesList += $line.Substring($contentIndent)
    } else {
      $linesList += ''
    }
    $script:lineIndex++
  }

  return ($linesList -join "`n")
}

$allCases = @()
foreach ($yamlFile in $yamlFiles) {
  $data = ConvertFrom-YamlFile -Path $yamlFile

  if ($data.passing) {
    foreach ($tc in $data.passing) {
      $isExclusive = $false
      if ($tc.source -match 'lib\\std|lib\\usr|ext\\std|ext\\usr|Add-StagedProbe|Copy-Item -Path|WRITEFILE\(|DELETEFILE\(|EXISTFILE\(|READFILE\(|TEMPFILE\(') {
        $isExclusive = $true
      }
      $allCases += [pscustomobject]@{
        YamlPath = $yamlFile
        Name = $tc.name
        Source = $tc.source
        Language = $tc.language
        ExpectedPass = $true
        Exclusive = $isExclusive
      }
    }
  }

  if ($data.failing) {
    foreach ($tc in $data.failing) {
      $isExclusive = $false
      if ($tc.source -match 'lib\\std|lib\\usr|ext\\std|ext\\usr|Add-StagedProbe|Copy-Item -Path|WRITEFILE\(|DELETEFILE\(|EXISTFILE\(|READFILE\(|TEMPFILE\(') {
        $isExclusive = $true
      }
      $allCases += [pscustomobject]@{
        YamlPath = $yamlFile
        Name = $tc.name
        Source = $tc.source
        Language = $tc.language
        ExpectedPass = $false
        Exclusive = $isExclusive
      }
    }
  }
}

$allCases = $allCases | Sort-Object YamlPath, Name

$total = $allCases.Count

$tempDir = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $tempDir | Out-Null

# Create a subdirectory structure that mimics tests/cases/passing/
# so $PSScriptRoot-based helper path resolution works
$execDir = Join-Path $tempDir 'cases\passing'
New-Item -ItemType Directory -Path $execDir | Out-Null

$resourcesDir = Join-Path $casesDir 'resources'
if (Test-Path $resourcesDir) {
    Copy-Item -Path (Join-Path $resourcesDir '*') -Destination $execDir -Recurse -Force | Out-Null
}

$helpersDir = Join-Path $testDir 'helpers'
if (Test-Path $helpersDir) {
    New-Item -ItemType Directory -Path (Join-Path $tempDir 'helpers') -Force | Out-Null
    Copy-Item -Path "$helpersDir\*" -Destination (Join-Path $tempDir 'helpers') -Recurse -Force -ErrorAction SilentlyContinue | Out-Null
}

# Copy prefix.exe to temp parent and runtime files so helper path resolution works
$tempParent = Split-Path -Parent $tempDir
foreach ($artifact in @('prefix.exe', 'prefix', 'prefix_runtime.dll', 'prefix_runtime.lib', 'libprefix_runtime.so')) {
    $src = Join-Path $prefixDir $artifact
    if (Test-Path $src) {
        Copy-Item -Path $src -Destination (Join-Path $tempParent $artifact) -Force
    }
}

# Pre-create lib/std/, lib/usr/, ext/std/, ext/usr/ dirs in temp for search-path tests
foreach ($sub in @('lib\std', 'lib\usr', 'ext\std', 'ext\usr')) {
    New-Item -ItemType Directory -Path (Join-Path $tempParent $sub) -Force | Out-Null
}

if ($total -eq 0) {
  Remove-Item -Path $tempDir -Recurse -Force
  $testEnd = Get-Date
  $elapsed = $testEnd - $testStart
  $elapsedSeconds = '{0:N3}' -f $elapsed.TotalSeconds
  Write-Host "Ran 0 tests in $elapsedSeconds seconds, with 0/0 tests passing, 0/0 tests failing." -ForegroundColor Green
  exit 0
}

Write-Host 'Running tests...'
$testStart = Get-Date

$maxThreads = [Math]::Max(1, [Environment]::ProcessorCount - 1)

$dispatchQueue = [System.Collections.Concurrent.BlockingCollection[object]]::new()
$resultsQueue = [System.Collections.Concurrent.ConcurrentQueue[object]]::new()

$powerShellExe = if ($PSVersionTable.PSEdition -eq 'Core') {
    if ($IsLinux -or $IsMacOS) { Join-Path $PSHOME 'pwsh' } else { Join-Path $PSHOME 'pwsh.exe' }
  } else {
    Join-Path $PSHOME 'powershell.exe'
  }

$rwLock = New-Object System.Threading.ReaderWriterLockSlim

$sharedState = [pscustomobject]@{
    ActiveExclusive = 0
    ActiveNonExclusive = 0
    Lock = [System.Object]::new()
}

$workerScript = @'
param($dispatchQueue, $resultsQueue, $exePath, $tempDir, $powerShellExe, $rwLock, $sharedState)

while (-not $dispatchQueue.IsCompleted) {
    $item = $null
    if (-not $dispatchQueue.TryTake([ref]$item, 100)) { continue }

    $extension = if ($item.Language -eq 'PowerShell') { '.ps1' } else { '.pre' }
    $execDir = Join-Path $tempDir 'cases\passing'
    $tempFile = Join-Path $execDir ([Guid]::NewGuid().ToString() + $extension)
    Set-Content -Path $tempFile -Value $item.Source -NoNewline

    if ($item.Exclusive) {
        $rwLock.EnterWriteLock()
        try {
            [System.Threading.Monitor]::Enter($sharedState.Lock)
            try { $sharedState.ActiveExclusive++ }
            finally { [System.Threading.Monitor]::Exit($sharedState.Lock) }
            $output = @()
            if ($extension -ieq '.ps1') {
                if (-not (Test-Path $powerShellExe)) { throw "PowerShell executable not found at: $powerShellExe" }
                $powerShellCommand = "& { `$ErrorActionPreference = 'Stop'; & `$args[0] }"
                $output = & $powerShellExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command $powerShellCommand $tempFile 2>&1
            } else {
                $output = & $exePath $tempFile 2>&1
            }
            $exitCode = [int]$LASTEXITCODE
            $resultsQueue.Enqueue([pscustomobject]@{
                YamlPath = $item.YamlPath
                Name = $item.Name
                ExitCode = $exitCode
                Output = (($output | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine)
                ExpectedPass = $item.ExpectedPass
            })
        }
        finally {
            $rwLock.ExitWriteLock()
            [System.Threading.Monitor]::Enter($sharedState.Lock)
            try { $sharedState.ActiveExclusive-- }
            finally { [System.Threading.Monitor]::Exit($sharedState.Lock) }
        }
    } else {
        [System.Threading.Monitor]::Enter($sharedState.Lock)
        try { $sharedState.ActiveNonExclusive++ }
        finally { [System.Threading.Monitor]::Exit($sharedState.Lock) }
        $rwLock.EnterReadLock()
        try {
            $output = @()
            if ($extension -ieq '.ps1') {
                if (-not (Test-Path $powerShellExe)) { throw "PowerShell executable not found at: $powerShellExe" }
                $powerShellCommand = "& { `$ErrorActionPreference = 'Stop'; & `$args[0] }"
                $output = & $powerShellExe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command $powerShellCommand $tempFile 2>&1
            } else {
                $output = & $exePath $tempFile 2>&1
            }
            $exitCode = [int]$LASTEXITCODE
            $resultsQueue.Enqueue([pscustomobject]@{
                YamlPath = $item.YamlPath
                Name = $item.Name
                ExitCode = $exitCode
                Output = (($output | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine)
                ExpectedPass = $item.ExpectedPass
            })
        }
        finally {
            $rwLock.ExitReadLock()
            [System.Threading.Monitor]::Enter($sharedState.Lock)
            try { $sharedState.ActiveNonExclusive-- }
            finally { [System.Threading.Monitor]::Exit($sharedState.Lock) }
        }
    }
}
'@

$runspacePool = [runspacefactory]::CreateRunspacePool(1, $maxThreads)
$runspacePool.Open()

$workers = @()
for ($i = 0; $i -lt $maxThreads; $i++) {
    $ps = [PowerShell]::Create()
    $ps.RunspacePool = $runspacePool
    $ps.AddScript($workerScript) | Out-Null
    [void]$ps.AddArgument($dispatchQueue)
    [void]$ps.AddArgument($resultsQueue)
    [void]$ps.AddArgument($exePath)
    [void]$ps.AddArgument($tempDir)
    [void]$ps.AddArgument($powerShellExe)
    [void]$ps.AddArgument($rwLock)
    [void]$ps.AddArgument($sharedState)
    $async = $ps.BeginInvoke()
    $workers += [pscustomobject]@{ PowerShell = $ps; Async = $async }
}

# Dispatcher: single thread handles scheduling/dequeuing with starvation guards
$starvationThresholdMs = 5000
$exclusivePendingSince = $null

foreach ($item in $allCases) {
    if ($item.Exclusive) {
        $exclusivePendingSince = Get-Date
    }

    if (-not $item.Exclusive -and $null -ne $exclusivePendingSince) {
        $elapsed = ((Get-Date) - $exclusivePendingSince).TotalMilliseconds
        if ($elapsed -gt $starvationThresholdMs) {
            do {
                Start-Sleep -Milliseconds 10
            } while ($sharedState.ActiveExclusive -gt 0 -or $sharedState.ActiveNonExclusive -gt 0)
            $exclusivePendingSince = $null
        }
    }

    $dispatchQueue.Add($item)
}
$dispatchQueue.CompleteAdding()

$lastDisplayedLength = 0
while ($true) {
    $completed = $resultsQueue.Count
    if ($completed -gt 0 -or $completed -eq 0) {
        $remaining = $total - $completed
        $message = "${completed}/${total} tests completed, ${remaining} remaining."
        $pad = if ($message.Length -lt $lastDisplayedLength) { ' ' * ($lastDisplayedLength - $message.Length) } else { '' }
        $lastDisplayedLength = $message.Length
        Write-Host -NoNewline "`r$message$pad"
    }
    if ($completed -ge $total) { break }
    Start-Sleep -Milliseconds 100
}

Write-Host ''

foreach ($w in $workers) {
    try { $w.PowerShell.EndInvoke($w.Async) } catch { }
    $w.PowerShell.Dispose()
}
$runspacePool.Close()
$runspacePool.Dispose()

Remove-Item -Path $tempDir -Recurse -Force

$failures = @()
$results = @()
$res = $null
while ($resultsQueue.TryDequeue([ref]$res)) { $results += $res }

foreach ($r in $results) {
    if ($r.ExpectedPass -and $r.ExitCode -ne 0) {
        $failures += [pscustomobject]@{
            YamlPath = $r.YamlPath
            Name = $r.Name
            Type = 'UnexpectedFailure'
            ExitCode = $r.ExitCode
            Output = $r.Output
        }
    } elseif (-not $r.ExpectedPass -and $r.ExitCode -eq 0) {
        $failures += [pscustomobject]@{
            YamlPath = $r.YamlPath
            Name = $r.Name
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
    $relativePath = $f.YamlPath.Replace($prefixDir, '').TrimStart('\', '/')
    Write-Host " - ${relativePath}: $($f.Name): $($f.Type)"
    if ($f.Output) { Write-Host $f.Output -ForegroundColor Red }
  }
  Write-Host ''
  Write-Host "Ran $total tests in $elapsedSeconds seconds, with $($total - $failures.Count)/$($total) tests passing, $($failures.Count)/$($total) tests failing." -ForegroundColor Red
  exit 1
} else {
  Write-Host "Ran $total tests in $elapsedSeconds seconds, with $($total)/$($total) tests passing, 0/$($total) tests failing." -ForegroundColor Green
  exit 0
}
