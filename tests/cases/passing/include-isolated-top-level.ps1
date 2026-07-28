$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
$moduleName = 'prefixinclude' + ([IO.Path]::GetRandomFileName().Replace('.', ''))

try {
  $programPath = Join-Path $tempDir 'program.pre'
  $modulePath = Join-Path $tempDir "$moduleName.pre"

  Set-Content -Path $modulePath -Encoding Ascii -Value @'
INT module_var = 0d7

FUNC INT read_module_var(){
    RETURN(module_var)
}
'@

  Set-Content -Path $programPath -Encoding Ascii -Value @"
INT caller_var = 0d11

INCLUDE("$moduleName")

ASSERT(EQ(read_module_var(), 0d7))
ASSERT(EQ(module_var, 0d7))
ASSERT(EQ(caller_var, 0d11))
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
