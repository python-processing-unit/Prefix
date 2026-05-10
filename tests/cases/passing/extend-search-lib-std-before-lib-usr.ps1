$helperPath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'helpers\prefix-input.ps1'
. $helperPath

$prefixDir = Get-PrefixRootPath
$tempDir = New-PrefixTempDir
$suffix = Get-PrefixExtensionSuffix
$probeSource = Get-PrefixHelperExtensionPath 'search_probe'
$stagedPaths = @()

function Add-StagedProbe([string]$Destination) {
  Copy-Item -Path $probeSource -Destination $Destination -Force
  $script:stagedPaths += $Destination
}

try {
  $programPath = Join-Path $tempDir 'program.pre'
  $libStdProbe = Join-Path (Join-Path $prefixDir 'lib\std') ("search_probe{0}" -f $suffix)
  $libUsrProbe = Join-Path (Join-Path $prefixDir 'lib\usr') ("search_probe{0}" -f $suffix)

  Add-StagedProbe $libStdProbe
  Add-StagedProbe $libUsrProbe

  Set-Content -Path $programPath -Encoding Ascii -Value @'
BOOL: loaded = EXTEND(search_probe)
REFUTE(loaded)
PRINT(TST_SEARCH_PROBE_PATH())
'@

  $result = Invoke-PrefixWithArguments -Arguments @($programPath)
  Assert-PrefixSuccess $result
  Assert-NoErrorOutput $result

  $actualPath = $result.Stdout.Trim()
  $expectedPath = [IO.Path]::GetFullPath($libStdProbe)
  if ([IO.Path]::GetFullPath($actualPath).ToLowerInvariant() -ne $expectedPath.ToLowerInvariant()) {
    throw "Expected EXTEND to prefer lib/std over lib/usr when ext roots are absent`nExpected: $expectedPath`nActual:   $actualPath"
  }
}
finally {
  foreach ($stagedPath in $stagedPaths) {
    Remove-Item -Path $stagedPath -Force -ErrorAction SilentlyContinue
  }

  Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}