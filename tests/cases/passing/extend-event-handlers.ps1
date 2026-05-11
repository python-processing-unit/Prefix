$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
$hooksExtension = Get-PrefixHelperExtensionPath 'extend_hooks'

try {
  $eventLogPath = Join-Path $tempDir 'events.log'
  $programPath = Join-Path $tempDir 'program.pre'

  Set-Content -Path $programPath -Encoding Ascii -Value @'
FUNC INT ping(){
    RETURN(0d1)
}

ASSERT(EQ(TST_HOOKS_READY(), "hooks-ready"))
ASSERT(EQ(ping(), 0d1))
'@

  $result = Invoke-PrefixWithArguments -Arguments @($hooksExtension, $programPath) -EnvironmentVariables @{
    PREFIX_TEST_EVENT_LOG = $eventLogPath
  }

  Assert-PrefixSuccess $result
  Assert-NoErrorOutput $result

  if (-not (Test-Path $eventLogPath)) {
    throw 'Expected the event-hook extension to create its log file'
  }

  $events = Get-Content -Path $eventLogPath | Where-Object { $_.Length -gt 0 }
  foreach ($expectedEvent in @('program_start', 'before_statement', 'after_statement', 'before_call', 'after_call', 'program_end')) {
    if ($events -notcontains $expectedEvent) {
      throw "Expected event '$expectedEvent' to appear in the hook log. Got: $($events -join ', ')"
    }
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}