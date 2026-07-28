$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

# Send a single command and EOF (no explicit .exit)
$result = Invoke-PrefixWithArguments -Arguments @() -InputText "PRINT('bye')`n"

Assert-PrefixSuccess $result
Assert-NoErrorOutput $result

$stdout = Format-VisibleText $result.Stdout
if (-not ($stdout -match 'bye')) {
  throw "Unexpected stdout: $stdout"
}
