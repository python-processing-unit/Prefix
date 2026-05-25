$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
$moduleName = 'prefiximport' + ([IO.Path]::GetRandomFileName().Replace('.', ''))

try {
  $programPath = Join-Path $tempDir 'program.pre'
  $modulePath = Join-Path $tempDir "$moduleName.pre"
  $pointerPath = Join-Path $tempDir "$moduleName.prex"

  Set-Content -Path $modulePath -Encoding Ascii -Value @'
FUNC STR identify(){
    RETURN("source")
}
'@

  Set-Content -Path $pointerPath -Encoding Ascii -Value 'this companion pointer file must be ignored by IMPORT'

  Set-Content -Path $programPath -Encoding Ascii -Value @"
IMPORT("$moduleName")

ASSERT(EQ($moduleName.identify(), "source"))
"@

  $output = & (Get-PrefixExePath) $programPath 2>&1
  if ($LASTEXITCODE -ne 0) {
    $outputText = ($output | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
    throw "Prefix exited with code $LASTEXITCODE`n$outputText"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}