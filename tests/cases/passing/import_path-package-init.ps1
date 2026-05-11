$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
$packageName = 'prefixpkg' + ([IO.Path]::GetRandomFileName().Replace('.', ''))

try {
  $programPath = Join-Path $tempDir 'program.pre'
  $packageDir = Join-Path $tempDir $packageName
  $initPath = Join-Path $packageDir 'init.pre'

  New-Item -ItemType Directory -Path $packageDir | Out-Null

  Set-Content -Path $initPath -Encoding Ascii -Value @'
FUNC STR identify(){
    RETURN("package")
}
'@

  $packageLiteral = $packageDir.Replace('\', '\\')

  Set-Content -Path $programPath -Encoding Ascii -Value @"
IMPORT_PATH("$packageLiteral")

ASSERT(EQ($packageName.identify(), "package"))
"@

  $result = Invoke-PrefixProgramWithInput -ProgramPath $programPath -InputText ''
  Assert-PrefixSuccess $result
  Assert-NoErrorOutput $result
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}