$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
$packageName = 'prefixpkg' + ([IO.Path]::GetRandomFileName().Replace('.', ''))

try {
  $programPath = Join-Path $tempDir 'program.pre'
  $modulePath = Join-Path $tempDir "$packageName.pre"
  $packageDir = Join-Path $tempDir $packageName

  New-Item -ItemType Directory -Path $packageDir | Out-Null

  Set-Content -Path $modulePath -Encoding Ascii -Value @'
FUNC STR identify(){
    RETURN("module")
}
'@

  $packageLiteral = $packageDir.Replace('\', '\\')

  Set-Content -Path $programPath -Encoding Ascii -Value @"
IMPORT_PATH("$packageLiteral")
"@

  $result = Invoke-PrefixProgramWithInput -ProgramPath $programPath -InputText ''
  if ($result.ExitCode -eq 0) {
    $outputText = "STDOUT: $(Format-VisibleText $result.Stdout)`nSTDERR: $(Format-VisibleText $result.Stderr)"
    throw "Expected IMPORT_PATH($packageLiteral) to fail when the package directory has no init.pre`n$outputText"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}