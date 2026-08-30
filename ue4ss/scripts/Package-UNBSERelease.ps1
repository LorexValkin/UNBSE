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
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    $text = $utf8.GetString([IO.File]::ReadAllBytes($source))
    $canonicalText = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    [IO.File]::WriteAllText($destination, $canonicalText, $utf8)
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

function Copy-DirectoryFiles {
    param(
        [Parameter(Mandatory)][string]$SourceDirectory,
        [Parameter(Mandatory)][string]$DestinationDirectory
    )

    $sourcePrefix = $SourceDirectory.TrimEnd(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    foreach ($file in Get-ChildItem -LiteralPath $SourceDirectory -File -Recurse) {
        $relative = $file.FullName.Substring($sourcePrefix.Length)
        $destination = Join-Path $DestinationDirectory $relative
        New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force |
            Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
    }
}

function Write-StageChecksums {
    param([Parameter(Mandatory)][string]$StageDirectory)

    $prefix = $StageDirectory.TrimEnd(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    $checksums = Get-ChildItem -LiteralPath $StageDirectory -File -Recurse |
        Where-Object { $_.Name -cne 'UNBSE-SHA256SUMS.txt' } |
        Sort-Object FullName |
        ForEach-Object {
            $relative = $_.FullName.Substring($prefix.Length).Replace('\', '/')
            "$(Get-UNBSEHash $_.FullName)  $relative"
        }
    [IO.File]::WriteAllLines(
        (Join-Path $StageDirectory 'UNBSE-SHA256SUMS.txt'),
        $checksums,
        [Text.UTF8Encoding]::new($false))
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
$dropInStage = Join-Path $stageRoot 'drop-in'
$runtimeStage = Join-Path $stageRoot 'runtime-only'
$modsStage = Join-Path $stageRoot 'mods-only'
$mo2Stage = Join-Path $stageRoot 'mo2'
$sourceStage = Join-Path $stageRoot 'source'

try {
    New-Item -ItemType Directory -Path $foundationStage, $dropInStage, $runtimeStage,
        $modsStage, $mo2Stage, $sourceStage -Force | Out-Null
    Expand-Archive -LiteralPath $foundationArchive -DestinationPath $foundationStage -Force

    foreach ($artifact in $manifest.requiredRuntimeArtifacts) {
        $packageRelative = if ($artifact.PSObject.Properties.Name -contains
            'packageRelativePath') {
            [string]$artifact.packageRelativePath
        } else { [string]$artifact.relativePath }
        Copy-VerifiedFile `
            -Source (Join-Path $foundationStage ([string]$artifact.relativePath)) `
            -Destination (Join-Path $runtimeStage $packageRelative) `
            -Sha256 ([string]$artifact.sha256) `
            -Bytes ([long]$artifact.bytes)
    }
    $requiredSettings = @{}
    $manifest.requiredSettings.psobject.Properties | ForEach-Object {
        $requiredSettings[$_.Name] = [string]$_.Value
    }
    Set-UNBSERequiredIni `
        -Path (Join-Path $runtimeStage 'ue4ss\UE4SS-settings.ini') `
        -RequiredSettings $requiredSettings
    foreach ($artifact in $manifest.requiredRuntimeArtifacts) {
        $packageRelative = if ($artifact.PSObject.Properties.Name -contains
            'packageRelativePath') {
            [string]$artifact.packageRelativePath
        } else { [string]$artifact.relativePath }
        $packageSha256 = if ($artifact.PSObject.Properties.Name -contains
            'packageSha256') {
            [string]$artifact.packageSha256
        } else { [string]$artifact.sha256 }
        $packageBytes = if ($artifact.PSObject.Properties.Name -contains
            'packageBytes') {
            [long]$artifact.packageBytes
        } else { [long]$artifact.bytes }
        $packagedFile = Assert-UNBSENoReparsePath (Join-Path $runtimeStage $packageRelative)
        if ((Get-Item -LiteralPath $packagedFile).Length -ne $packageBytes -or
            (Get-UNBSEHash $packagedFile) -cne $packageSha256.ToUpperInvariant()) {
            throw "Packaged runtime artifact differs from its pin: $packageRelative"
        }
    }
    foreach ($artifact in $packageManifest.artifacts) {
        $relative = [string]$artifact.relativePath
        $destinationRoot = if ($relative.StartsWith(
            'ue4ss/Mods/', [StringComparison]::Ordinal)) { $modsStage } else { $runtimeStage }
        Copy-VerifiedFile `
            -Source (Join-Path $corePackage $relative) `
            -Destination (Join-Path $destinationRoot $relative) `
            -Sha256 ([string]$artifact.sha256) `
            -Bytes ([long]$artifact.bytes)
    }
    $installedManifest = Join-Path $modsStage 'ue4ss\Mods\UNBSE\unbse-mod-manifest.json'
    New-Item -ItemType Directory -Path (Split-Path $installedManifest -Parent) -Force |
        Out-Null
    Copy-Item -LiteralPath $packageManifestPath -Destination $installedManifest -Force
    foreach ($stage in @($runtimeStage, $modsStage)) {
        Copy-Item -LiteralPath (Join-Path $repositoryRoot 'README.md') `
            -Destination (Join-Path $stage 'UNBSE-README.md') -Force
    }

    Copy-DirectoryFiles -SourceDirectory $runtimeStage -DestinationDirectory $dropInStage
    Copy-DirectoryFiles -SourceDirectory $modsStage -DestinationDirectory $dropInStage

    $mo2RuntimeRoot = Join-Path $mo2Stage 'Root\OblivionRemastered\Binaries\Win64'
    $runtimePrefix = $runtimeStage.TrimEnd(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    foreach ($file in Get-ChildItem -LiteralPath $runtimeStage -File -Recurse) {
        $relative = $file.FullName.Substring($runtimePrefix.Length).Replace('\', '/')
        if ($relative -ceq 'dwmapi.dll' -or $relative -ceq 'UNBSE-README.md') {
            continue
        }
        $destination = if ($relative.StartsWith('ue4ss/Mods/',
                [StringComparison]::Ordinal)) {
            Join-Path $mo2Stage ('UE4SS/' + $relative.Substring('ue4ss/Mods/'.Length))
        } else {
            Join-Path $mo2RuntimeRoot $relative
        }
        New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force |
            Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
    }
    $modsPrefix = $modsStage.TrimEnd(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    foreach ($file in Get-ChildItem -LiteralPath $modsStage -File -Recurse) {
        $relative = $file.FullName.Substring($modsPrefix.Length).Replace('\', '/')
        if ($relative -ceq 'UNBSE-README.md') { continue }
        if (-not $relative.StartsWith('ue4ss/Mods/', [StringComparison]::Ordinal)) {
            throw "Unexpected mods-only package path: $relative"
        }
        $destination = Join-Path $mo2Stage (
            'UE4SS/' + $relative.Substring('ue4ss/Mods/'.Length))
        New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force |
            Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
    }
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'README.md') `
        -Destination (Join-Path $mo2Stage 'UNBSE-README.md') -Force

    foreach ($stage in @($dropInStage, $runtimeStage, $modsStage, $mo2Stage)) {
        Write-StageChecksums -StageDirectory $stage
    }

    $sourcePaths = @(
        @($manifest.unbseMod.sourceFiles | ForEach-Object { [string]$_.relativePath }) +
        @($manifest.unbseMod.obse64Interop.sourceFiles |
            ForEach-Object { [string]$_.relativePath }) +
        @($manifest.patchSet.patches | ForEach-Object { [string]$_.relativePath }) +
        @('.gitattributes', 'README.md', 'ue4ss/foundation-manifest.json')
    ) | Sort-Object -Unique
    foreach ($relativePath in $sourcePaths) {
        Copy-RepositoryFile `
            -RepositoryRoot $repositoryRoot `
            -RelativePath $relativePath `
            -DestinationRoot $sourceStage
    }

    $dropInZip = Join-Path $output "UNBSE-$version.zip"
    $runtimeZip = Join-Path $output "UNBSE-$version-runtime.zip"
    $modsZip = Join-Path $output "UNBSE-$version-mods.zip"
    $mo2Zip = Join-Path $output "UNBSE-$version-mo2.zip"
    $sourceZip = Join-Path $output "UNBSE-$version-source.zip"
    New-ZipFromDirectory -SourceDirectory $dropInStage -DestinationPath $dropInZip
    New-ZipFromDirectory -SourceDirectory $runtimeStage -DestinationPath $runtimeZip
    New-ZipFromDirectory -SourceDirectory $modsStage -DestinationPath $modsZip
    New-ZipFromDirectory -SourceDirectory $mo2Stage -DestinationPath $mo2Zip
    New-ZipFromDirectory -SourceDirectory $sourceStage -DestinationPath $sourceZip
    $releaseChecksums = @($dropInZip, $runtimeZip, $modsZip, $mo2Zip, $sourceZip |
        ForEach-Object {
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
        DropInArchive = $dropInZip
        RuntimeArchive = $runtimeZip
        ModsArchive = $modsZip
        ModOrganizer2Archive = $mo2Zip
        SourceArchive = $sourceZip
        Checksums = $checksumPath
    } | ConvertTo-Json -Compress
}
finally {
    if (Test-Path -LiteralPath $stageRoot) {
        Remove-Item -LiteralPath $stageRoot -Recurse -Force
    }
}
