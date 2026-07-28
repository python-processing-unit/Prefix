function Get-PrefixExePath {
  $testsDir = Split-Path -Parent $PSScriptRoot
  $prefixDir = Split-Path -Parent $testsDir
  $exeName = if ($IsLinux -or $IsMacOS) { 'prefix' } else { 'prefix.exe' }
  $exePath = Join-Path $prefixDir $exeName

  if (-not (Test-Path $exePath)) {
    throw "Interpreter executable not found at: $exePath"
  }

  return $exePath
}

function Get-PrefixRootPath {
  $testsDir = Split-Path -Parent $PSScriptRoot
  return (Split-Path -Parent $testsDir)
}

function Get-PrefixExtensionSuffix {
  $platform = [System.Runtime.InteropServices.RuntimeInformation]

  if ($platform::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Linux)) {
    return '.so'
  }

  if ($platform::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::OSX)) {
    return '.dylib'
  }

  return '.dll'
}

function Get-PrefixHelperExtensionPath {
  param(
    [Parameter(Mandatory = $true)]
    [string]$RelativePath
  )

  $artifactPath = Join-Path $PSScriptRoot ($RelativePath + (Get-PrefixExtensionSuffix))
  if (-not (Test-Path $artifactPath)) {
    throw "Helper extension artifact not found at: $artifactPath"
  }

  return $artifactPath
}

function New-PrefixTempDir {
  $tempDir = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
  New-Item -ItemType Directory -Path $tempDir | Out-Null
  return $tempDir
}

function ConvertTo-PrefixArgumentString {
  param(
    [Parameter(Mandatory = $false)]
    [string[]]$Arguments = @()
  )
  if (-not $Arguments) { return '' }

  $escaped = foreach ($argument in $Arguments) {
    if ($null -eq $argument) {
      '""'
      continue
    }

    '"{0}"' -f ($argument.Replace('"', '\"'))
  }

  return ($escaped -join ' ')
}

function Start-PrefixProcessWithArguments {
  param(
    [Parameter(Mandatory = $false)]
    [string[]]$Arguments = @(),

    [string]$WorkingDirectory,

    [hashtable]$EnvironmentVariables
  )

  $startInfo = New-Object System.Diagnostics.ProcessStartInfo
  $startInfo.FileName = Get-PrefixExePath
  $startInfo.Arguments = ConvertTo-PrefixArgumentString -Arguments $Arguments
  $startInfo.RedirectStandardInput = $true
  $startInfo.RedirectStandardOutput = $true
  $startInfo.RedirectStandardError = $true
  $startInfo.UseShellExecute = $false
  $startInfo.CreateNoWindow = $true

  if ($WorkingDirectory) {
    $startInfo.WorkingDirectory = $WorkingDirectory
  }

  if ($EnvironmentVariables) {
    foreach ($entry in $EnvironmentVariables.GetEnumerator()) {
      if ($null -eq $entry.Value) {
        [void]$startInfo.EnvironmentVariables.Remove($entry.Key)
      } else {
        $startInfo.EnvironmentVariables[$entry.Key] = [string]$entry.Value
      }
    }
  }

  return [System.Diagnostics.Process]::Start($startInfo)
}

function Start-PrefixProcess {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ProgramPath
  )

  if (-not (Test-Path $ProgramPath)) {
    throw "Program not found at: $ProgramPath"
  }

  return Start-PrefixProcessWithArguments -Arguments @($ProgramPath)
}

function Invoke-PrefixWithArguments {
  param(
    [Parameter(Mandatory = $false)]
    [string[]]$Arguments = @(),

    [AllowEmptyString()]
    [string]$InputText = '',

    [string]$WorkingDirectory,

    [hashtable]$EnvironmentVariables
  )

  $process = Start-PrefixProcessWithArguments -Arguments $Arguments -WorkingDirectory $WorkingDirectory -EnvironmentVariables $EnvironmentVariables
  try {
    try {
      $process.StandardInput.Write($InputText)
      $process.StandardInput.Close()
    } catch [System.IO.IOException] {
      # On Linux, writing to stdin after the process has exited throws
      # "Broken pipe". This is expected when a REPL handler or early exit
      # terminates the process before stdin is consumed.
    }

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

function Invoke-PrefixProgramWithInput {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ProgramPath,

    [Parameter(Mandatory = $true)]
    [AllowEmptyString()]
    [string]$InputText
  )

  return Invoke-PrefixWithArguments -Arguments @($ProgramPath) -InputText $InputText
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
