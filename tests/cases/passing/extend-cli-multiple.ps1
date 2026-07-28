$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
$coreExtension = Get-PrefixHelperExtensionPath 'extend_core'
$packageExtension = Get-PrefixHelperExtensionPath 'extend_package\init'

try {
  $programPath = Join-Path $tempDir 'program.pre'

  Set-Content -Path $programPath -Encoding Ascii -Value @'
ASSERT(EQ(TST_EXT_GLOBAL(), "core-global"))
ASSERT(EQ(TST_EXT_PACKAGE_INIT(), "package-init"))
'@

  $result = Invoke-PrefixWithArguments -Arguments @($coreExtension, $packageExtension, $programPath)
  Assert-PrefixSuccess $result
  Assert-NoErrorOutput $result

  if ($result.Stdout.Length -ne 0) {
    $stdout = Format-VisibleText $result.Stdout
    throw "Expected no stdout output, got: $stdout"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}