$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$prefixDir = Get-PrefixRootPath
$tempDir = New-PrefixTempDir
$programDir = Join-Path $tempDir 'program'
$cwdDir = Join-Path $tempDir 'cwd'
$suffix = Get-PrefixExtensionSuffix
$probeSource = Get-PrefixHelperExtensionPath 'search_probe'
$stagedPaths = @()

function Add-StagedProbe([string]$Destination) {
  Copy-Item -Path $probeSource -Destination $Destination -Force
  $script:stagedPaths += $Destination
}

try {
  New-Item -ItemType Directory -Path $programDir | Out-Null
  New-Item -ItemType Directory -Path $cwdDir | Out-Null

  $programPath = Join-Path $programDir 'program.pre'
  $programProbe = Join-Path $programDir ("search_probe{0}" -f $suffix)
  $cwdProbe = Join-Path $cwdDir ("search_probe{0}" -f $suffix)
  $extStdProbe = Join-Path (Join-Path $prefixDir 'ext\std') ("search_probe{0}" -f $suffix)
  $extUsrProbe = Join-Path (Join-Path $prefixDir 'ext\usr') ("search_probe{0}" -f $suffix)
  $libStdProbe = Join-Path (Join-Path $prefixDir 'lib\std') ("search_probe{0}" -f $suffix)
  $libUsrProbe = Join-Path (Join-Path $prefixDir 'lib\usr') ("search_probe{0}" -f $suffix)

  Add-StagedProbe $programProbe
  Add-StagedProbe $cwdProbe
  Add-StagedProbe $extStdProbe
  Add-StagedProbe $extUsrProbe
  Add-StagedProbe $libStdProbe
  Add-StagedProbe $libUsrProbe

  Set-Content -Path $programPath -Encoding Ascii -Value @'
BOOL: loaded = EXTEND(search_probe)
ASSERT(NOT(loaded))
PRINT(TST_SEARCH_PROBE_PATH())
'@

  $result = Invoke-PrefixWithArguments -Arguments @($programPath) -WorkingDirectory $cwdDir
  Assert-PrefixSuccess $result
  Assert-NoErrorOutput $result

  $actualPath = $result.Stdout.Trim()
  $expectedPath = [IO.Path]::GetFullPath($programProbe)
  if ([IO.Path]::GetFullPath($actualPath).ToLowerInvariant() -ne $expectedPath.ToLowerInvariant()) {
    throw "Expected EXTEND to resolve from the program directory`nExpected: $expectedPath`nActual:   $actualPath"
  }
}
finally {
  foreach ($stagedPath in $stagedPaths) {
    Remove-Item -Path $stagedPath -Force -ErrorAction SilentlyContinue
  }

  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}