$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir

try {
  $programPath = Join-Path $tempDir 'program.pre'

  Set-Content -Path $programPath -Encoding Ascii -Value @'
STR line = INPUT()
ASSERT(ISSTR(line))
ASSERT(EQ(line, "Alpha beta 42"))
'@

  $result = Invoke-PrefixProgramWithInput -ProgramPath $programPath -InputText "Alpha beta 42`r`nignored`r`n"

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
