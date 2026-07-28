$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
try {
  $programPath = Join-Path $tempDir 'program.pre'

  Set-Content -Path $programPath -Encoding Ascii -Value @'
FREEZE("not_declared")
'@

  # Pass both -verbose and -private; private should suppress snapshots
  $result = Invoke-PrefixWithArguments -Arguments @('-verbose', '-private', $programPath)

  if ($result.ExitCode -eq 0) {
    $stdout = Format-VisibleText $result.Stdout
    $stderr = Format-VisibleText $result.Stderr
    throw "Expected interpreter to exit nonzero; got 0. STDOUT: $stdout STDERR: $stderr"
  }

  if (-not ($result.Stderr -match 'Traceback')) {
    $stderr = Format-VisibleText $result.Stderr
    throw "Expected 'Traceback' in stderr: $stderr"
  }

  if ($result.Stderr -match 'State log index') {
    $stderr = Format-VisibleText $result.Stderr
    throw "Unexpected state log when -private present: $stderr"
  }

  if ($result.Stderr -match 'Env snapshot') {
    $stderr = Format-VisibleText $result.Stderr
    throw "Unexpected env snapshot when -private present: $stderr"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
