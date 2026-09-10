<#
.SYNOPSIS
    Runs the CI job locally, from a clean clone, including the [graphics] cases
    that a hosted runner cannot run.
.DESCRIPTION
    This is the other half of .github/workflows/ci.yml, not a replacement for
    it. The two cover different things and neither is sufficient alone:

      hosted CI (.github/workflows/ci.yml)
          Clean checkout on a machine that is not yours, every push. Catches the
          class of defect .docs/PROGRESS.md records three times -- a stale or
          wrong build output tree producing a failure that reads as a source
          defect. Cannot run the 45 [graphics] cases: GitHub-hosted runners have
          no D3D12 adapter, and a self-hosted runner is not an option because
          this repository is public (a fork's pull request would execute on the
          runner's machine).

      this script
          Clean clone plus a real GPU, so it runs all 221 cases. Slower, manual,
          and run before pushing rather than after.

    "Clean clone" is the load-bearing word. Building in the working tree
    verifies the source; cloning to a scratch directory first also verifies that
    the source is all that is needed -- no stale x64\ output, no
    wrong-configuration vcpkg DLL left over from an earlier session, no
    generated file that only exists on this machine.

.PARAMETER Configuration
    Debug, Release, or Both (default: Both). Both is the meaningful default --
    the /MDd-versus-/MD split between configurations is where this repository's
    most expensive environment defect lived.
.PARAMETER IncludeUncommitted
    Also apply the working tree's uncommitted changes to tracked files into the
    clone. Without this the clone is exactly HEAD, which is what CI will see
    after a push.
.PARAMETER InPlace
    Skip the clone and run against the working tree. Faster, and appropriate
    while iterating, but it does not check the clean-checkout property, which is
    most of the point.
.PARAMETER WorkDirectory
    Where to place the clone (default: a new directory under $env:TEMP).
.PARAMETER KeepWorkDirectory
    Do not delete the clone afterwards. Implied when the run fails, so there is
    something left to inspect.
.EXAMPLE
    .\scripts\ci-local.ps1
.EXAMPLE
    .\scripts\ci-local.ps1 -IncludeUncommitted -Configuration Debug
.EXAMPLE
    .\scripts\ci-local.ps1 -InPlace -Configuration Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'Both')]
    [string]$Configuration = 'Both',

    [switch]$IncludeUncommitted,

    [switch]$InPlace,

    [string]$WorkDirectory,

    [switch]$KeepWorkDirectory
)

. (Join-Path $PSScriptRoot 'common.ps1')

$repositoryRoot = Get-Preview3DRepositoryRoot
$startedAt = Get-Date
$cloneRoot = $null
$succeeded = $false

function Write-Section {
    param([string]$Text)
    Write-Host ''
    Write-Host "=== $Text" -ForegroundColor Magenta
}

try {
    # ---------------------------------------------------------------- clone --
    if ($InPlace) {
        Write-Section 'Using the working tree (-InPlace); clean-checkout property NOT verified'
        $treeRoot = $repositoryRoot
    }
    else {
        if (-not $WorkDirectory) {
            $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
            $WorkDirectory = Join-Path $env:TEMP "preview3d-ci-$stamp"
        }
        $cloneRoot = Join-Path $WorkDirectory 'repo'

        Write-Section "Clean clone -> $cloneRoot"

        $headCommit = (& git -C $repositoryRoot rev-parse HEAD).Trim()
        $headBranch = (& git -C $repositoryRoot rev-parse --abbrev-ref HEAD).Trim()
        Write-Host "HEAD: $headCommit ($headBranch)"

        New-Item -ItemType Directory -Path $WorkDirectory -Force | Out-Null

        # --no-hardlinks so the clone's object store cannot alias the real one.
        & git clone --quiet --no-hardlinks --no-checkout $repositoryRoot $cloneRoot
        if ($LASTEXITCODE -ne 0) { throw "git clone failed with exit code $LASTEXITCODE." }

        & git -C $cloneRoot checkout --quiet --detach $headCommit
        if ($LASTEXITCODE -ne 0) { throw "git checkout failed with exit code $LASTEXITCODE." }

        if ($IncludeUncommitted) {
            $patchPath = Join-Path $WorkDirectory 'uncommitted.patch'
            # --binary so fixture bytes survive; HEAD so staged and unstaged
            # changes to tracked files are both included. Untracked files are
            # deliberately excluded: CI will not have them either.
            & git -C $repositoryRoot diff --binary HEAD | Set-Content -LiteralPath $patchPath -Encoding utf8NoBOM
            if ((Get-Item -LiteralPath $patchPath).Length -gt 0) {
                Write-Host "Applying uncommitted changes to tracked files."
                & git -C $cloneRoot apply --whitespace=nowarn $patchPath
                if ($LASTEXITCODE -ne 0) { throw "Failed to apply uncommitted changes (exit code $LASTEXITCODE)." }
            }
            else {
                Write-Host 'No uncommitted changes to tracked files.'
            }

            $untracked = & git -C $repositoryRoot ls-files --others --exclude-standard
            if ($untracked) {
                Write-Warning "Untracked files are NOT copied into the clone; CI will not see them either:"
                $untracked | ForEach-Object { Write-Warning "    $_" }
            }
        }

        $treeRoot = $cloneRoot
    }

    # ---------------------------------------------------------------- build --
    $configurations = if ($Configuration -eq 'Both') { @('Debug', 'Release') } else { @($Configuration) }
    $buildScript = Join-Path $treeRoot 'scripts\build.ps1'
    $testScript = Join-Path $treeRoot 'scripts\run-tests.ps1'
    $resultsDirectory = Join-Path $repositoryRoot 'TestResults\ci-local'

    foreach ($config in $configurations) {
        Write-Section "Build $config"
        & $buildScript -Configuration $config
        if ($LASTEXITCODE -ne 0) { throw "Build failed for $config." }
    }

    # ----------------------------------------------------------------- test --
    # No tag filter here, unlike the hosted job: this machine has a real D3D12
    # adapter, so the 45 [graphics] cases the hosted runner skips do run.
    foreach ($config in $configurations) {
        Write-Section "Test $config (full suite, including [graphics])"
        & $testScript -NoBuild -Configuration $config -ResultsDirectory $resultsDirectory
        if ($LASTEXITCODE -ne 0) { throw "Tests failed for $config." }
    }

    $succeeded = $true
}
finally {
    $elapsed = (Get-Date) - $startedAt
    Write-Host ''

    if ($succeeded) {
        Write-Host ("=== ci-local PASSED in {0:mm\:ss}" -f $elapsed) -ForegroundColor Green
    }
    else {
        Write-Host ("=== ci-local FAILED after {0:mm\:ss}" -f $elapsed) -ForegroundColor Red
    }

    if ($cloneRoot -and (Test-Path -LiteralPath $cloneRoot)) {
        if ($succeeded -and -not $KeepWorkDirectory) {
            Write-Host "Removing clone at $WorkDirectory"
            Remove-Item -LiteralPath $WorkDirectory -Recurse -Force -ErrorAction SilentlyContinue
        }
        else {
            Write-Host "Clone kept for inspection: $cloneRoot"
        }
    }
}

if (-not $succeeded) { exit 1 }
exit 0
