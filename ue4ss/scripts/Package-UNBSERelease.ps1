[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$FoundationArchivePath,
    [string]$CorePackageRoot = (Join-Path $PSScriptRoot '..\..\out\ue4ss-package'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\..\release'),
    [string]$ManifestPath = (Join-Path $PSScriptRoot '..\foundation-manifest.json')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Import-Module (Join-Path $PSScriptRoot 'UNBSEUE4SSFoundation.psm1') -Force

function Copy-VerifiedFile {
    param(
        [Parameter(Mandatory)][string]$Source,
        [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][string]$Sha256,
        [Parameter(Mandatory)][long]$Bytes
    )

    $sourceFile = Assert-UNBSENoReparsePath $Source
    if ((Get-Item -LiteralPath $sourceFile).Length -ne $Bytes) {
        throw "Unexpected file length: $Source"
    }
    if ((Get-UNBSEHash $sourceFile) -cne $Sha256.ToUpperInvariant()) {
        throw "Unexpected SHA-256: $Source"
    }
    New-Item -ItemType Directory -Path (Split-Path $Destination -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $sourceFile -Destination $Destination -Force
}

function Copy-RepositoryFile {
    param(
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [Parameter(Mandatory)][string]$RelativePath,
        [Parameter(Mandatory)][string]$DestinationRoot
    )

    $source = Assert-UNBSENoReparsePath (Join-Path $RepositoryRoot $RelativePath)
    $destination = Join-Path $DestinationRoot $RelativePath
    New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
}

function New-ZipFromDirectory {
    param(
        [Parameter(Mandatory)][string]$SourceDirectory,
        [Parameter(Mandatory)][string]$DestinationPath
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path -LiteralPath $DestinationPath) {
        Remove-Item -LiteralPath $DestinationPath -Force
    }
    [IO.Compression.ZipFile]::CreateFromDirectory(
        $SourceDirectory,
        $DestinationPath,
        [IO.Compression.CompressionLevel]::Optimal,
        $false)
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$manifestFile = Assert-UNBSENoReparsePath $ManifestPath
$foundationArchive = Assert-UNBSENoReparsePath $FoundationArchivePath
$corePackage = Assert-UNBSENoReparsePath $CorePackageRoot
$manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
$version = [string]$manifest.unbseMod.version
if ($version -cne '0.11.0-rc.1') {
    throw "Unexpected release version: $version"
}
if ((Get-Item -LiteralPath $foundationArchive).Length -ne
    [long]$manifest.sourceDistribution.archiveBytes -or
    (Get-UNBSEHash $foundationArchive) -cne
    [string]$manifest.sourceDistribution.archiveSha256) {
    throw 'The UE4SS foundation archive does not match the pinned release input.'
}

$packageManifestPath = Assert-UNBSENoReparsePath (
    Join-Path $corePackage 'unbse-mod-manifest.json')
$packageManifest = Get-Content -LiteralPath $packageManifestPath -Raw | ConvertFrom-Json
if ([string]$packageManifest.version -cne $version -or
    [string]$packageManifest.foundationId -cne [string]$manifest.foundationId) {
    throw 'The core package does not match the release manifest.'
}

$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$stageRoot = Join-Path $repositoryRoot "out\release-stage-$([guid]::NewGuid().ToString('N'))"
$foundationStage = Join-Path $stageRoot 'foundation'
$runtimeStage = Join-Path $stageRoot 'runtime'
$sourceStage = Join-Path $stageRoot 'source'

try {
    New-Item -ItemType Directory -Path $foundationStage, $runtimeStage, $sourceStage -Force |
        Out-Null
    Expand-Archive -LiteralPath $foundationArchive -DestinationPath $foundationStage -Force

    foreach ($artifact in $manifest.requiredRuntimeArtifacts) {
        Copy-VerifiedFile `
            -Source (Join-Path $foundationStage ([string]$artifact.relativePath)) `
            -Destination (Join-Path $runtimeStage ([string]$artifact.relativePath)) `
            -Sha256 ([string]$artifact.sha256) `
            -Bytes ([long]$artifact.bytes)
    }
    foreach ($artifact in $packageManifest.artifacts) {
        Copy-VerifiedFile `
            -Source (Join-Path $corePackage ([string]$artifact.relativePath)) `
            -Destination (Join-Path $runtimeStage ([string]$artifact.relativePath)) `
            -Sha256 ([string]$artifact.sha256) `
            -Bytes ([long]$artifact.bytes)
    }
    $installedManifest = Join-Path $runtimeStage 'ue4ss\Mods\UNBSE\unbse-mod-manifest.json'
    New-Item -ItemType Directory -Path (Split-Path $installedManifest -Parent) -Force |
        Out-Null
    Copy-Item -LiteralPath $packageManifestPath -Destination $installedManifest -Force
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'README.md') `
        -Destination (Join-Path $runtimeStage 'UNBSE-README.md') -Force

    $runtimePrefix = $runtimeStage.TrimEnd(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    $runtimeChecksums = Get-ChildItem -LiteralPath $runtimeStage -File -Recurse |
        Sort-Object FullName |
        ForEach-Object {
            $relative = $_.FullName.Substring($runtimePrefix.Length).Replace('\', '/')
            "$(Get-UNBSEHash $_.FullName)  $relative"
        }
    [IO.File]::WriteAllLines(
        (Join-Path $runtimeStage 'UNBSE-SHA256SUMS.txt'),
        $runtimeChecksums,
        [Text.UTF8Encoding]::new($false))

    $sourcePaths = @(
        @($manifest.unbseMod.sourceFiles | ForEach-Object { [string]$_.relativePath }) +
        @($manifest.unbseMod.obse64Interop.sourceFiles |
            ForEach-Object { [string]$_.relativePath }) +
        @($manifest.patchSet.patches | ForEach-Object { [string]$_.relativePath }) +
        @('README.md', 'ue4ss/foundation-manifest.json')
    ) | Sort-Object -Unique
    foreach ($relativePath in $sourcePaths) {
        Copy-RepositoryFile `
            -RepositoryRoot $repositoryRoot `
            -RelativePath $relativePath `
            -DestinationRoot $sourceStage
    }

    $runtimeZip = Join-Path $output "UNBSE-$version.zip"
    $sourceZip = Join-Path $output "UNBSE-$version-source.zip"
    New-ZipFromDirectory -SourceDirectory $runtimeStage -DestinationPath $runtimeZip
    New-ZipFromDirectory -SourceDirectory $sourceStage -DestinationPath $sourceZip
    $releaseChecksums = @($runtimeZip, $sourceZip | ForEach-Object {
        "$(Get-UNBSEHash $_)  $([IO.Path]::GetFileName($_))"
    })
    $checksumPath = Join-Path $output 'SHA256SUMS.txt'
    [IO.File]::WriteAllLines(
        $checksumPath,
        $releaseChecksums,
        [Text.UTF8Encoding]::new($false))

    [pscustomobject]@{
        Success = $true
        Version = $version
        RuntimeArchive = $runtimeZip
        SourceArchive = $sourceZip
        Checksums = $checksumPath
    } | ConvertTo-Json -Compress
}
finally {
    if (Test-Path -LiteralPath $stageRoot) {
        Remove-Item -LiteralPath $stageRoot -Recurse -Force
    }
}
