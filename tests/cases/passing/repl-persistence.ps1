$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$input = @'
BOOL: persisted = TRUE

PRINT(persisted)
.exit
'@

$result = Invoke-PrefixWithArguments -Arguments @() -InputText $input

Assert-PrefixSuccess $result
Assert-NoErrorOutput $result

$stdout = Format-VisibleText $result.Stdout
if (-not ($stdout -match 'TRUE')) {
  throw "Unexpected stdout: $stdout"
}
