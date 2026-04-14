function Get-PrefixExePath {
  $testsDir = Split-Path -Parent $PSScriptRoot
  $prefixDir = Split-Path -Parent $testsDir
  $exePath = Join-Path $prefixDir 'prefix.exe'

  if (-not (Test-Path $exePath)) {
    throw "Interpreter executable not found at: $exePath"
  }

  return $exePath
}

function New-PrefixTempDir {
  $tempDir = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
  New-Item -ItemType Directory -Path $tempDir | Out-Null
  return $tempDir
}

function Start-PrefixProcess {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ProgramPath
  )

  if (-not (Test-Path $ProgramPath)) {
    throw "Program not found at: $ProgramPath"
  }

  $startInfo = New-Object System.Diagnostics.ProcessStartInfo
  $startInfo.FileName = Get-PrefixExePath
  $startInfo.Arguments = ('"{0}"' -f $ProgramPath)
  $startInfo.RedirectStandardInput = $true
  $startInfo.RedirectStandardOutput = $true
  $startInfo.RedirectStandardError = $true
  $startInfo.UseShellExecute = $false
  $startInfo.CreateNoWindow = $true

  return [System.Diagnostics.Process]::Start($startInfo)
}

function Invoke-PrefixProgramWithInput {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ProgramPath,

    [Parameter(Mandatory = $true)]
    [AllowEmptyString()]
    [string]$InputText
  )

  $process = Start-PrefixProcess -ProgramPath $ProgramPath
  try {
    $process.StandardInput.Write($InputText)
    $process.StandardInput.Close()

    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()

    return [pscustomobject]@{
      ExitCode = $process.ExitCode
      Stdout = $stdout
      Stderr = $stderr
    }
  }
  finally {
    $process.Dispose()
  }
}

function Format-VisibleText([string]$Text) {
  return $Text.Replace("`r", '\r').Replace("`n", '\n')
}

function Assert-PrefixSuccess($Result) {
  if ($Result.ExitCode -ne 0) {
    $stdout = Format-VisibleText $Result.Stdout
    $stderr = Format-VisibleText $Result.Stderr
    throw "Prefix exited with code $($Result.ExitCode)`nSTDOUT: $stdout`nSTDERR: $stderr"
  }
}

function Assert-NoErrorOutput($Result) {
  if ($Result.Stderr.Length -ne 0) {
    $stderr = Format-VisibleText $Result.Stderr
    throw "Expected no stderr output, got: $stderr"
  }
}
