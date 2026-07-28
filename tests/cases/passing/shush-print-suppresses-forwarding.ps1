$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$tempDir = New-PrefixTempDir

try {
  $programPath = Join-Path $tempDir 'program.pre'

  Set-Content -Path $programPath -Encoding Ascii -Value @'
SHUSH()
PRINT("hidden")
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

  Assert-PrefixSuccess $result
  Assert-NoErrorOutput $result

  if ($result.Stdout.Length -ne 0) {
    $stdout = Format-VisibleText $result.Stdout
    throw "Expected no stdout output, got: $stdout"
  }
}
finally {
  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
