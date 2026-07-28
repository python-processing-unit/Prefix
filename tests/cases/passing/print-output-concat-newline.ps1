$prefixDir = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$exePath = Join-Path $prefixDir 'prefix.exe'

if (-not (Test-Path $exePath)) {
  throw "Interpreter executable not found at: $exePath"
}

$tempDir = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $programPath = Join-Path $tempDir 'program.pre'

  Set-Content -Path $programPath -Encoding Ascii -Value @'
PRINT("left=", TRUE, ",right=", 0d42, ",flt=", INF)
'@

  $startInfo = New-Object System.Diagnostics.ProcessStartInfo
  $startInfo.FileName = $exePath
  $startInfo.Arguments = ('"{0}"' -f $programPath)
  $startInfo.RedirectStandardOutput = $true
  $startInfo.RedirectStandardError = $true
  $startInfo.UseShellExecute = $false
  $startInfo.CreateNoWindow = $true

  $process = [System.Diagnostics.Process]::Start($startInfo)
  $stdout = $process.StandardOutput.ReadToEnd()
  $stderr = $process.StandardError.ReadToEnd()
  $process.WaitForExit()
  $exitCode = $process.ExitCode
  $process.Dispose()

  if ($exitCode -ne 0) {
    throw "Prefix exited with code $exitCode`nSTDOUT:`n$stdout`nSTDERR:`n$stderr"
  }

  if ($stderr.Length -ne 0) {
    throw "Expected no stderr output, got:`n$stderr"
  }

  if (-not [regex]::IsMatch($stdout, '\Aleft=TRUE,right=0d42,flt=INF(?:\r\n|\n)\z')) {
    $escapedStdout = $stdout.Replace("`r", '\r').Replace("`n", '\n')
    throw "Unexpected stdout: $escapedStdout"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}