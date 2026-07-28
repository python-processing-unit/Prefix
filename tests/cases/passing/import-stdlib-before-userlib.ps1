$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$prefixDir = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$tempDir = New-PrefixTempDir
$moduleName = 'prefiximport' + ([IO.Path]::GetRandomFileName().Replace('.', ''))
$stdlibPath = Join-Path $prefixDir "lib\std\$moduleName.pre"
$userlibPath = Join-Path $prefixDir "lib\usr\$moduleName.pre"

try {
  $programPath = Join-Path $tempDir 'program.pre'

  Set-Content -Path $stdlibPath -Encoding Ascii -Value @'
FUNC STR identify(){
    RETURN("stdlib")
}
'@

  Set-Content -Path $userlibPath -Encoding Ascii -Value @'
FUNC STR identify(){
    RETURN("userlib")
}
'@

  Set-Content -Path $programPath -Encoding Ascii -Value @"
IMPORT("$moduleName")

ASSERT(EQ($moduleName.identify(), "stdlib"))
"@

  $output = & (Get-PrefixExePath) $programPath 2>&1
  if ($LASTEXITCODE -ne 0) {
    $outputText = ($output | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
    throw "Prefix exited with code $LASTEXITCODE`n$outputText"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item -Path $stdlibPath -Force -ErrorAction SilentlyContinue
  Remove-Item -Path $userlibPath -Force -ErrorAction SilentlyContinue
}