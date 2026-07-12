param()

$ErrorActionPreference = 'Stop'
$failures = 0

function Test-RequiredCommand {
    param([string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        Write-Host "[missing] $Name"
        $script:failures++
        return $null
    }

    Write-Host "[ok] $Name`: $($command.Source)"
    return $command
}

Write-Host 'Windows MSVC client baseline'
$null = Test-RequiredCommand git
$null = Test-RequiredCommand msbuild
$cmake = Test-RequiredCommand cmake
$null = Test-RequiredCommand msgfmt
$null = Test-RequiredCommand make

if ($null -ne $cmake) {
    $versionLine = (cmake --version | Select-Object -First 1)
    if ($versionLine -match 'cmake version ([0-9]+)[.]') {
        if ([int]$Matches[1] -ge 4) {
            Write-Host "[missing] CMake 3.x required; found $versionLine"
            $failures++
        } else {
            Write-Host "[ok] $versionLine"
        }
    }
}

$solution = Join-Path $PSScriptRoot '..\msvc-full-features\Cataclysm-vcpkg-static.sln'
if (Test-Path $solution) {
    Write-Host "[ok] MSVC solution: $solution"
} else {
    Write-Host "[missing] MSVC solution: $solution"
    $failures++
}

if ($env:VCPKG_ROOT -and (Test-Path (Join-Path $env:VCPKG_ROOT 'vcpkg.exe'))) {
    Write-Host "[ok] vcpkg: $env:VCPKG_ROOT"
} else {
    Write-Host '[warn] VCPKG_ROOT is not configured; CI installs the pinned vcpkg revision automatically.'
}

if ($failures -ne 0) {
    throw "Environment check failed with $failures missing requirement(s)."
}

Write-Host 'Environment check passed.'
