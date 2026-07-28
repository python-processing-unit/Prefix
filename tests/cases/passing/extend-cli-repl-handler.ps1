$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
$hooksExtension = Get-PrefixHelperExtensionPath 'extend_hooks'

try {
  $replLogPath = Join-Path $tempDir 'repl.log'

  $result = Invoke-PrefixWithArguments -Arguments @($hooksExtension) -InputText ".exit`n" -EnvironmentVariables @{
    PREFIX_TEST_REPL_LOG = $replLogPath
  }

  Assert-PrefixSuccess $result
  Assert-NoErrorOutput $result

  if (-not (Test-Path $replLogPath)) {
    throw 'Expected the replacement REPL handler to create its marker file'
  }

  $marker = Get-Content -Path $replLogPath -Raw
  if ($marker -ne 'repl-handler') {
    throw "Unexpected REPL marker text: $marker"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}