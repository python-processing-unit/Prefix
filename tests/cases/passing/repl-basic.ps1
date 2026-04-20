$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$result = Invoke-PrefixWithArguments -Arguments @() -InputText "PRINT('repl-basic')`n.exit`n"

Assert-PrefixSuccess $result
Assert-NoErrorOutput $result

$stdout = Format-VisibleText $result.Stdout
if (-not ($stdout -match 'repl-basic')) {
  throw "Unexpected stdout: $stdout"
}
