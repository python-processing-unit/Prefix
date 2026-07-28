$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir

try {
  $programPath = Join-Path $tempDir 'program.pre'

  Set-Content -Path $programPath -Encoding Ascii -Value @'
STR line = INPUT("PROMPT>")
ASSERT(EQ(line, "alpha"))
PRINT("DONE=", line)
'@

  $result = Invoke-PrefixProgramWithInput -ProgramPath $programPath -InputText "alpha`r`n"

  Assert-PrefixSuccess $result
  Assert-NoErrorOutput $result

  if (-not [regex]::IsMatch($result.Stdout, '\APROMPT>DONE=alpha(?:\r\n|\n)\z')) {
    $stdout = Format-VisibleText $result.Stdout
    throw "Unexpected stdout: $stdout"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
