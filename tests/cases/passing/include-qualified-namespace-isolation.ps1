$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
$moduleName = 'prefixinclude' + ([IO.Path]::GetRandomFileName().Replace('.', ''))

try {
  $programPath = Join-Path $tempDir 'program.pre'
  $modulePath = Join-Path $tempDir "$moduleName.pre"

  Set-Content -Path $modulePath -Encoding Ascii -Value @'
INT hidden = 0d7
'@

  Set-Content -Path $programPath -Encoding Ascii -Value @"
INCLUDE("$moduleName")

ASSERT(EQ($moduleName.hidden, 0d7))
"@

  $result = Invoke-PrefixProgramWithInput -ProgramPath $programPath -InputText ''
  if ($result.ExitCode -eq 0) {
    $outputText = "STDOUT: $(Format-VisibleText $result.Stdout)`nSTDERR: $(Format-VisibleText $result.Stderr)"
    throw "Expected qualified access to fail after INCLUDE($moduleName)`n$outputText"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
