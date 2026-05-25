$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
$suffix = Get-PrefixExtensionSuffix
$coreExtension = Get-PrefixHelperExtensionPath 'extend_core'

try {
  $programPath = Join-Path $tempDir 'program.pre'
  $stagedExtension = Join-Path $tempDir ("extend_core{0}" -f $suffix)
  $extensionSpecifier = "extend_core{0}" -f $suffix

  Copy-Item -Path $coreExtension -Destination $stagedExtension -Force

  Set-Content -Path $programPath -Encoding Ascii -Value @"
BOOL loaded = EXTEND("$extensionSpecifier")
REFUTE(loaded)
"@

  $result = Invoke-PrefixWithArguments -Arguments @($programPath)
  if ($result.ExitCode -eq 0) {
    $stdout = Format-VisibleText $result.Stdout
    $stderr = Format-VisibleText $result.Stderr
    throw "Expected EXTEND to reject a suffix-qualified specifier`nSTDOUT: $stdout`nSTDERR: $stderr"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}