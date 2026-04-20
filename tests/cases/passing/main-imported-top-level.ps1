$prefixDir = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$exePath = Join-Path $prefixDir 'prefix.exe'

if (-not (Test-Path $exePath)) {
  throw "Interpreter executable not found at: $exePath"
}

$tempDir = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $modulePath = Join-Path $tempDir 'main_top_result.pre'
  $programPath = Join-Path $tempDir 'program.pre'

  Set-Content -Path $modulePath -Encoding Ascii -Value @'
BOOL: top_result = MAIN()

FUNC BOOL: read_top_result(){
    RETURN(top_result)
}
'@

  $moduleLiteral = $modulePath.Replace('\', '\\')

  Set-Content -Path $programPath -Encoding Ascii -Value @"
IMPORT_PATH("$moduleLiteral", helper)

ASSERT(NOT(helper.read_top_result()))
ASSERT(EQ(TYPE(helper.read_top_result()), "BOOL"))
"@

  $output = & $exePath $programPath 2>&1
  if ($LASTEXITCODE -ne 0) {
    $outputText = ($output | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
    throw "Prefix exited with code $LASTEXITCODE`n$outputText"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}