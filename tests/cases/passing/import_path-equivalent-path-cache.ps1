$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
$moduleName = 'prefiximportpath' + ([IO.Path]::GetRandomFileName().Replace('.', ''))

try {
  $markerPath = Join-Path $tempDir 'marker.txt'
  $programPath = Join-Path $tempDir 'program.pre'
  $modulePath = Join-Path $tempDir "$moduleName.pre"
  $extensionlessPath = Join-Path $tempDir $moduleName

  $markerLiteral = $markerPath.Replace('\', '/')
  $moduleLiteral = $modulePath.Replace('\', '\\')
  $extensionlessLiteral = $extensionlessPath.Replace('\', '\\')

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
IMPORT_PATH("$extensionlessLiteral")
IMPORT_PATH("$moduleLiteral", again)

ASSERT(EQ($moduleName.marker_text(), "loaded"))
ASSERT(EQ(again.marker_text(), "loaded"))
"@

  $result = Invoke-PrefixProgramWithInput -ProgramPath $programPath -InputText ''
  Assert-PrefixSuccess $result
  Assert-NoErrorOutput $result
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}