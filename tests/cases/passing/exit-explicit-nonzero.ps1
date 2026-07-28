$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir

try {
  $programPath = Join-Path $tempDir 'program.pre'

  Set-Content -Path $programPath -Encoding Ascii -Value @'
PRINT("custom")
EXIT(0d23)
PRINT("after")
'@

  $startInfo = New-Object System.Diagnostics.ProcessStartInfo
  $startInfo.FileName = Get-PrefixExePath
  $startInfo.Arguments = ('"{0}"' -f $programPath)
  $startInfo.RedirectStandardOutput = $true
  $startInfo.RedirectStandardError = $true
  $startInfo.UseShellExecute = $false
  $startInfo.CreateNoWindow = $true

  $process = [System.Diagnostics.Process]::Start($startInfo)
  try {
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()

    $result = [pscustomobject]@{
      ExitCode = $process.ExitCode
      Stdout = $stdout
      Stderr = $stderr
    }
  }
  finally {
    $process.Dispose()
  }

  if ($result.ExitCode -ne 23) {
    $stdout = Format-VisibleText $result.Stdout
    $stderr = Format-VisibleText $result.Stderr
    throw "Expected Prefix to exit with code 23, got $($result.ExitCode)`nSTDOUT: $stdout`nSTDERR: $stderr"
  }

  Assert-NoErrorOutput $result

  if (-not [regex]::IsMatch($result.Stdout, '\Acustom(?:\r\n|\n)\z')) {
    $stdout = Format-VisibleText $result.Stdout
    throw "Unexpected stdout: $stdout"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
