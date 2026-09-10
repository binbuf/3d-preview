<#
.SYNOPSIS
    Builds Preview3D.slnx and runs every Catch2 suite.
.DESCRIPTION
    The single entry point for running this repository's tests, used
    identically by CI and by a developer's machine. Before this existed both
    Catch2 binaries were launched by hand out of x64\<Config>\, which is how
    Gate 0's CI deliverable stayed open while 221 test cases accumulated.

    Two suites run here:

      Tests.Unit                -- platform primitives, wire format, and the
                                   D3D12 device/queue/swapchain/upload-ring
                                   stack. Needs a D3D12 adapter for its
                                   [graphics] cases.
      Tests.ImportIsolation     -- the AppContainer import sandbox, the broker
                                   protocol, the shared-section validator, the
                                   format adapters, and the hostile-worker
                                   containment suite. Launches real sandboxed
                                   child processes and creates an AppContainer
                                   profile per run.

    Results are written as JUnit XML so CI can surface individual failures
    rather than just a red step.
.PARAMETER Configuration
    Debug, Release, or Both (default: Both). Both is the meaningful default:
    the /MDd-vs-/MD split between configurations is exactly where this
    repository's most expensive environment defect lived.
.PARAMETER Suite
    All (default), Unit, or ImportIsolation.
.PARAMETER NoBuild
    Run the existing binaries without building first.
.PARAMETER ResultsDirectory
    Where to write JUnit XML (default: TestResults\ at the repository root).
.PARAMETER CatchArgs
    Extra arguments forwarded verbatim to each Catch2 binary, e.g. a tag
    filter: -CatchArgs '[gltf-import]'
.EXAMPLE
    .\scripts\run-tests.ps1
.EXAMPLE
    .\scripts\run-tests.ps1 -Configuration Debug -Suite ImportIsolation -CatchArgs '[sandbox]'
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'Both')]
    [string]$Configuration = 'Both',

    [ValidateSet('All', 'Unit', 'ImportIsolation')]
    [string]$Suite = 'All',

    [switch]$NoBuild,

    [string]$ResultsDirectory,

    [string[]]$CatchArgs = @()
)

. (Join-Path $PSScriptRoot 'common.ps1')

$repositoryRoot = Get-Preview3DRepositoryRoot
if (-not $ResultsDirectory) {
    $ResultsDirectory = Join-Path $repositoryRoot 'TestResults'
}
if (-not (Test-Path -LiteralPath $ResultsDirectory)) {
    New-Item -ItemType Directory -Path $ResultsDirectory -Force | Out-Null
}

$configurations = if ($Configuration -eq 'Both') { @('Debug', 'Release') } else { @($Configuration) }
$suiteNames = switch ($Suite) {
    'All'             { @('Tests.Unit', 'Tests.ImportIsolation') }
    'Unit'            { @('Tests.Unit') }
    'ImportIsolation' { @('Tests.ImportIsolation') }
}

if (-not $NoBuild) {
    $msbuildPath = Find-Preview3DMSBuild
    Write-Host "MSBuild: $msbuildPath"
    foreach ($config in $configurations) {
        Invoke-Preview3DBuild -Configuration $config -MSBuildPath $msbuildPath
    }
}

$results = @()

foreach ($config in $configurations) {
    $outputDirectory = Get-Preview3DOutputDirectory -Configuration $config

    foreach ($suiteName in $suiteNames) {
        $exePath = Join-Path $outputDirectory "$suiteName.exe"
        if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
            throw "Test binary not found at '$exePath'. Build the solution first, or drop -NoBuild."
        }

        $junitPath = Join-Path $ResultsDirectory "$suiteName.$config.junit.xml"

        Write-Host ''
        Write-Host "==> $suiteName ($config|x64)" -ForegroundColor Cyan

        # Catch2 v3 multi-reporter form: human-readable console output *and* a
        # machine-readable file from one run. '::out=-' is stdout.
        $arguments = @(
            '--reporter', 'console::out=-'
            '--reporter', "junit::out=$junitPath"
            '--order', 'decl'
        ) + $CatchArgs

        # Run from the output directory so the binary resolves its app-local
        # vcpkg DLLs and its sibling worker executables the same way it does
        # when launched by hand.
        Push-Location $outputDirectory
        try {
            & $exePath @arguments
            $exitCode = $LASTEXITCODE
        }
        finally {
            Pop-Location
        }

        $results += [pscustomobject]@{
            Suite         = $suiteName
            Configuration = $config
            ExitCode      = $exitCode
            JUnit         = $junitPath
        }
    }
}

Write-Host ''
Write-Host '==> Summary' -ForegroundColor Cyan
$results | Format-Table -AutoSize Suite, Configuration, ExitCode

$failed = @($results | Where-Object { $_.ExitCode -ne 0 })
if ($failed.Count -gt 0) {
    foreach ($failure in $failed) {
        Write-Host "FAILED: $($failure.Suite) ($($failure.Configuration)) exit code $($failure.ExitCode)" -ForegroundColor Red
    }
    exit 1
}

Write-Host "All suites passed. JUnit XML in $ResultsDirectory" -ForegroundColor Green
exit 0
