# Shared helpers for the repository-level build and test scripts.
#
# Dot-source this, do not run it:
#
#     . (Join-Path $PSScriptRoot 'common.ps1')
#
# Everything here exists so that CI and a developer's machine drive the build
# through exactly one code path. .docs/PROGRESS.md records three separate
# occasions where a build-environment difference produced a failure that read
# as a source defect; two of them (a project-level build, and a stale
# output directory) are impossible to hit through these scripts.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The repository root, derived from this file's location rather than from the
# caller's working directory.
$script:Preview3DRepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Get-Preview3DRepositoryRoot {
    return $script:Preview3DRepositoryRoot
}

function Get-Preview3DSolutionPath {
    return (Join-Path $script:Preview3DRepositoryRoot 'Preview3D.slnx')
}

<#
.SYNOPSIS
    Locates MSBuild.exe, preferring one already on PATH and falling back to
    vswhere.
.DESCRIPTION
    Requires a Visual Studio installation carrying the v145 platform toolset
    pinned in Directory.Build.props. The toolset pin is NFR-12
    (reproducibility) and is not relaxed to make a build succeed.
#>
function Find-Preview3DMSBuild {
    $command = Get-Command 'MSBuild.exe' -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswherePath) {
        $path = & $vswherePath `
            -latest `
            -prerelease `
            -products '*' `
            -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\MSBuild.exe' |
            Select-Object -First 1

        if ($path) {
            return $path
        }
    }

    throw 'MSBuild was not found. Install Visual Studio with the "Desktop development with C++" workload and the v145 platform toolset.'
}

<#
.SYNOPSIS
    Builds Preview3D.slnx for one configuration.
.DESCRIPTION
    Always builds the *solution*. Building an individual .vcxproj leaves
    $(SolutionDir) undefined, which bakes a nonexistent worker path into
    Tests.ImportIsolation's PREVIEW3D_IMPORT_WORKER_EXE define and fails 85 of
    its cases in a way that looks exactly like a broken sandbox. See
    .docs/PROGRESS.md, "Building a .vcxproj directly makes 85 of 135
    import-isolation tests fail".
#>
function Invoke-Preview3DBuild {
    param(
        [Parameter(Mandatory)]
        [ValidateSet('Debug', 'Release')]
        [string]$Configuration,

        [ValidateSet('Build', 'Rebuild', 'Clean')]
        [string]$Target = 'Build',

        [string]$MSBuildPath
    )

    if (-not $MSBuildPath) {
        $MSBuildPath = Find-Preview3DMSBuild
    }

    $solutionPath = Get-Preview3DSolutionPath
    if (-not (Test-Path -LiteralPath $solutionPath -PathType Leaf)) {
        throw "Solution not found at '$solutionPath'."
    }

    Write-Host "==> $Target Preview3D.slnx ($Configuration|x64)" -ForegroundColor Cyan

    & $MSBuildPath `
        $solutionPath `
        "/t:$Target" `
        "/p:Configuration=$Configuration" `
        '/p:Platform=x64' `
        '/m' `
        '/nologo' `
        '/v:minimal' `
        '/consoleLoggerParameters:Summary'

    if ($LASTEXITCODE -ne 0) {
        throw "$Target failed for $Configuration|x64 with exit code $LASTEXITCODE."
    }
}

<#
.SYNOPSIS
    Returns the shared output directory for a configuration.
.DESCRIPTION
    Every project in the solution shares $(SolutionDir)x64\$(Configuration).
    Test binaries must be run from here, never from a per-project output tree.
#>
function Get-Preview3DOutputDirectory {
    param(
        [Parameter(Mandatory)]
        [ValidateSet('Debug', 'Release')]
        [string]$Configuration
    )

    return (Join-Path $script:Preview3DRepositoryRoot "x64\$Configuration")
}
