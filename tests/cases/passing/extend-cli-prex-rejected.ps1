$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir
$prexPath = Join-Path (Split-Path -Parent $helperPath) 'extend_prex_only.prex'

try {
  $programPath = Join-Path $tempDir 'program.pre'

  Set-Content -Path $programPath -Encoding Ascii -Value @'
ASSERT(TRUE)
'@

  $result = Invoke-PrefixWithArguments -Arguments @($prexPath, $programPath)
  if ($result.ExitCode -eq 0) {
    $stdout = Format-VisibleText $result.Stdout
    $stderr = Format-VisibleText $result.Stderr
    throw "Expected the interpreter to reject a .prex CLI extension input`nSTDOUT: $stdout`nSTDERR: $stderr"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}