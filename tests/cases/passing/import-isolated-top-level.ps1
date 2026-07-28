$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
$moduleName = 'prefiximport' + ([IO.Path]::GetRandomFileName().Replace('.', ''))

try {
  $programPath = Join-Path $tempDir 'program.pre'
  $modulePath = Join-Path $tempDir "$moduleName.pre"

  Set-Content -Path $modulePath -Encoding Ascii -Value @'
INT shared = 0d7

FUNC INT read_shared(){
    RETURN(shared)
}
'@

  Set-Content -Path $programPath -Encoding Ascii -Value @"
INT shared = 0d11

IMPORT("$moduleName", "helper")

ASSERT(EQ(helper.read_shared(), 0d7))
ASSERT(EQ(shared, 0d11))
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