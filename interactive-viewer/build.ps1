[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Release,
    [switch]$Run
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$configuration = if ($Release) { 'Release' } else { 'Debug' }
$platform = 'x64'
$projectRoot = $PSScriptRoot
$projectPath = Join-Path $projectRoot 'Preview3D.vcxproj'
$executablePath = Join-Path $projectRoot "$platform\$configuration\Preview3D.exe"

function Find-MSBuild {
    $command = Get-Command 'MSBuild.exe' -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswherePath) {
        $path = & $vswherePath `
            -latest `
            -products '*' `
            -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\MSBuild.exe' |
            Select-Object -First 1

        if ($path) {
            return $path
        }
    }

    throw 'MSBuild was not found. Install Visual Studio with the Desktop development with C++ workload.'
}

$msbuildPath = Find-MSBuild
$commonArguments = @(
    $projectPath
    "/p:Configuration=$configuration"
    "/p:Platform=$platform"
    '/m'
    '/nologo'
)

if ($Clean) {
    Write-Host "Cleaning Preview3D ($configuration|$platform)..."
    & $msbuildPath @commonArguments '/t:Clean'
    if ($LASTEXITCODE -ne 0) {
        throw "Clean failed with exit code $LASTEXITCODE."
    }
}

Write-Host "Building Preview3D ($configuration|$platform)..."
& $msbuildPath @commonArguments '/t:Build'
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

Write-Host "Build succeeded: $executablePath"

if ($Run) {
    if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
        throw "Built executable was not found at '$executablePath'."
    }

    Write-Host 'Starting Preview3D...'
    Start-Process -FilePath $executablePath -WorkingDirectory $projectRoot
}
