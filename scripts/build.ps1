<#
.SYNOPSIS
    Builds Preview3D.slnx for x64.
.DESCRIPTION
    The repository-level build entry point. Always builds the solution, never
    an individual .vcxproj -- see scripts/common.ps1 for why that distinction
    is load-bearing.
.PARAMETER Configuration
    Debug, Release, or Both (default: Debug).
.PARAMETER Target
    Build (default), Rebuild, or Clean.
.EXAMPLE
    .\scripts\build.ps1 -Configuration Both
.EXAMPLE
    .\scripts\build.ps1 -Configuration Release -Target Rebuild
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'Both')]
    [string]$Configuration = 'Debug',

    [ValidateSet('Build', 'Rebuild', 'Clean')]
    [string]$Target = 'Build'
)

. (Join-Path $PSScriptRoot 'common.ps1')

$configurations = if ($Configuration -eq 'Both') { @('Debug', 'Release') } else { @($Configuration) }
$msbuildPath = Find-Preview3DMSBuild
Write-Host "MSBuild: $msbuildPath"

foreach ($config in $configurations) {
    Invoke-Preview3DBuild -Configuration $config -Target $Target -MSBuildPath $msbuildPath
}

Write-Host "==> $Target succeeded for: $($configurations -join ', ')" -ForegroundColor Green
