[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$requiredEnvironment = @(
    'VCPKG_ROOT',
    'PAPERBREAK_QT_ROOT',
    'OpenCV_DIR',
    'PAPERBREAK_MVS_ROOT',
    'PAPERBREAK_MVS_RUNTIME_DIR'
)
foreach ($name in $requiredEnvironment) {
    $value = [Environment]::GetEnvironmentVariable($name)
    if ([string]::IsNullOrWhiteSpace($value)) {
        throw "Required CI environment variable '$name' is not set."
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'vswhere.exe was not found; Visual Studio 2026 is required.'
}
$visualStudioRoot = & $vswhere -latest -products '*' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($visualStudioRoot)) {
    throw 'Visual Studio 2026 Community was not found.'
}
$developerShell = Join-Path $visualStudioRoot 'Common7\Tools\Launch-VsDevShell.ps1'
& $developerShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation

cmake --version
& (Join-Path $env:VCPKG_ROOT 'vcpkg.exe') version
& (Join-Path $env:PAPERBREAK_QT_ROOT 'bin\qtpaths6.exe') --qt-version

foreach ($preset in @('windows-vs2026-debug', 'windows-vs2026-release')) {
    cmake --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "Configure failed for $preset." }

    cmake --build --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "Build failed for $preset." }

    $reportDirectory = Join-Path 'out\test-results' $preset
    New-Item -ItemType Directory -Force -Path $reportDirectory | Out-Null
    ctest --preset $preset --output-junit (Join-Path $reportDirectory 'ctest.xml')
    if ($LASTEXITCODE -ne 0) { throw "CTest failed for $preset." }
}

cmake --build --preset windows-vs2026-debug --target format-check
if ($LASTEXITCODE -ne 0) { throw 'Formatting check failed.' }

cmake --preset windows-vs2026-static-analysis
if ($LASTEXITCODE -ne 0) { throw 'Static-analysis configuration failed.' }
cmake --build --preset windows-vs2026-static-analysis
if ($LASTEXITCODE -ne 0) { throw 'MSVC static analysis failed.' }
