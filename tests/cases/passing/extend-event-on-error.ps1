$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
$hooksExtension = Get-PrefixHelperExtensionPath 'extend_hooks'

try {
  $eventLogPath = Join-Path $tempDir 'events.log'
  $programPath = Join-Path $tempDir 'program.pre'

  Set-Content -Path $programPath -Encoding Ascii -Value @'
THROW("boom")
'@

  $result = Invoke-PrefixWithArguments -Arguments @($hooksExtension, $programPath) -EnvironmentVariables @{
    PREFIX_TEST_EVENT_LOG = $eventLogPath
  }

  if ($result.ExitCode -eq 0) {
    throw 'Expected the program to fail so the on_error hook can run'
  }

  if (-not (Test-Path $eventLogPath)) {
    throw 'Expected the event-hook extension to create its log file'
  }

  $events = Get-Content -Path $eventLogPath | Where-Object { $_.Length -gt 0 }
  if ($events -notcontains 'on_error') {
    throw "Expected event 'on_error' to appear in the hook log. Got: $($events -join ', ')"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}