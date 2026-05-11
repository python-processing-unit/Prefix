$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
$moduleName = 'prefiximport' + ([IO.Path]::GetRandomFileName().Replace('.', ''))

try {
  $markerPath = Join-Path $tempDir 'marker.txt'
  $markerLiteral = $markerPath.Replace('\', '/')
  $programPath = Join-Path $tempDir 'program.pre'
  $modulePath = Join-Path $tempDir "$moduleName.pre"

  Set-Content -Path $modulePath -Encoding Ascii -Value @"
STR marker = "$markerLiteral"

IF(EXISTFILE(marker)){
    WRITEFILE("reloaded", marker)
} ELSE {
    WRITEFILE("loaded", marker)
}

FUNC STR marker_text(){
    RETURN(READFILE(marker))
}
"@

  Set-Content -Path $programPath -Encoding Ascii -Value @"
IMPORT($moduleName)
IMPORT($moduleName, again)

ASSERT(EQ($moduleName.marker_text(), "loaded"))
ASSERT(EQ(again.marker_text(), "loaded"))
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