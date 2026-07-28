$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
$hooksExtension = Get-PrefixHelperExtensionPath 'extend_hooks'

try {
  $periodicLogPath = Join-Path $tempDir 'periodic.log'
  $programPath = Join-Path $tempDir 'program.pre'

  Set-Content -Path $programPath -Encoding Ascii -Value @'
INT total = 0d0

FOR(i, 0d40){
  total = +(total, i)
}

ASSERT(GT(total, 0d0))
'@

  $result = Invoke-PrefixWithArguments -Arguments @($hooksExtension, $programPath) -EnvironmentVariables @{
    PREFIX_TEST_PERIODIC_LOG = $periodicLogPath
  }

  Assert-PrefixSuccess $result
  Assert-NoErrorOutput $result

  if (-not (Test-Path $periodicLogPath)) {
    throw 'Expected the periodic-hook extension to create its log file'
  }

  $ticks = Get-Content -Path $periodicLogPath | Where-Object { $_ -eq 'periodic' }
  if ($ticks.Count -lt 2) {
    throw "Expected at least two periodic hook executions, got $($ticks.Count)"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}