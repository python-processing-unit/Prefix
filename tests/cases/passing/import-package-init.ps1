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

  Set-Content -Path $programPath -Encoding Ascii -Value @"
IMPORT($packageName)

ASSERT(EQ($packageName.identify(), "package"))
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