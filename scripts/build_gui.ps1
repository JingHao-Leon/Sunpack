[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64')][string]$Arch = 'x64',
    [string]$CMakeCommand = 'cmake',
    [string]$BuildDir,
    [int]$Jobs = [Environment]::ProcessorCount,
    [switch]$SkipTests
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot ".cache\gui-$Arch" }
$platform = if ($Arch -eq 'arm64') { 'ARM64' } else { 'x64' }
& $CMakeCommand -S (Join-Path $repoRoot 'gui') -B $BuildDir -A $platform
if ($LASTEXITCODE -ne 0) { throw 'GUI CMake configuration failed' }
& $CMakeCommand --build $BuildDir --config Release --target SunPackGUI sunpack_gui_tests sunpack_gui_ui_tests --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'GUI compilation failed' }
$hostArch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
if (-not $SkipTests -and $hostArch -eq $Arch) {
    $ctest = Join-Path (Split-Path -Parent (Get-Command $CMakeCommand).Source) 'ctest.exe'
    & $ctest --test-dir $BuildDir -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'GUI tests failed' }
    $python = Join-Path $repoRoot '.venv\Scripts\python.exe'
    if (Test-Path -LiteralPath $python) {
        $previousTestBinary = $env:SUNPACK_GUI_TEST_BINARY
        $env:SUNPACK_GUI_TEST_BINARY = Join-Path $BuildDir 'Release\sunpack_gui_tests.exe'
        try {
            & $python -m pytest (Join-Path $repoRoot 'tests\gui\test_integration.py') -q
            if ($LASTEXITCODE -ne 0) { throw 'GUI cross-language contracts failed' }
        } finally {
            if ($null -ne $previousTestBinary) { $env:SUNPACK_GUI_TEST_BINARY = $previousTestBinary }
            else { Remove-Item Env:SUNPACK_GUI_TEST_BINARY }
        }
    }
}
$binary = Get-Item -LiteralPath (Join-Path $BuildDir 'Release\SunPackGUI.exe')
Write-Host ('SunPackGUI ({0}): {1:N0} bytes ({2:N2} MiB)' -f $Arch, $binary.Length, ($binary.Length / 1MB))
