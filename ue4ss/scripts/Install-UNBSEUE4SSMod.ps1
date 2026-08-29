[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$GameRoot,
    [Parameter(Mandatory)][string]$PackageRoot,
    [string]$FoundationSourceRoot = (Join-Path $PSScriptRoot '..\..\..\UE4SS'),
    [string]$FoundationManifestPath = (Join-Path $PSScriptRoot '..\foundation-manifest.json'),
    [switch]$ForcePostStageFailure
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'UNBSEUE4SSMod.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'UNBSEUE4SSFoundation.psm1') -Force

$pre = Get-UNBSEUE4SSModAudit `
    -GameRoot $GameRoot `
    -PackageRoot $PackageRoot `
    -FoundationSourceRoot $FoundationSourceRoot `
    -FoundationManifestPath $FoundationManifestPath
if (-not $pre.FoundationSuccess -or -not $pre.PackageSuccess) {
    $pre | Select-Object Success, FoundationSuccess, PackageSuccess, InstalledSuccess, Errors |
        ConvertTo-Json -Compress -Depth 5
    exit 1
}
if ($pre.InstalledSuccess) {
    [pscustomobject]@{
        Success = $true
        Changed = $false
        Status = 'already-installed'
        GameRoot = $pre.GameRoot
    } | ConvertTo-Json -Compress
    exit 0
}

$game = Assert-UNBSENoReparsePath $pre.GameRoot
$package = Assert-UNBSENoReparsePath $pre.PackageRoot
$legacyInteropDisableRelative = `
    'ue4ss/Mods/UNBSEOBSE64Interop/enabled.txt.unbse-oracle-disabled'
$legacyInteropDisableMarker = Join-Path $game $legacyInteropDisableRelative
$legacyInteropDisablePresent = Test-Path -LiteralPath $legacyInteropDisableMarker -PathType Leaf
if ($legacyInteropDisablePresent) {
    Assert-UNBSENoReparsePath $legacyInteropDisableMarker | Out-Null
    if ((Get-Item -LiteralPath $legacyInteropDisableMarker).Length -ne 0 -or
        (Get-UNBSEHash $legacyInteropDisableMarker) -cne
            'E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855') {
        throw 'The legacy property-oracle OBSE64 disable marker is not the exact zero-byte marker.'
    }
    $activeInteropEnableMarker = Join-Path $game `
        'ue4ss\Mods\UNBSEOBSE64Interop\enabled.txt'
    if (Test-Path -LiteralPath $activeInteropEnableMarker) {
        throw 'Both active and legacy-disabled OBSE64 enable markers are present.'
    }
}
$ownedModRoots = @(
    [pscustomobject]@{ Relative = 'ue4ss/Mods/UNBSE/'; Path = (Join-Path $game 'ue4ss\Mods\UNBSE'); HasManifest = $true },
    [pscustomobject]@{ Relative = 'ue4ss/Mods/UNBSEOBSE64Interop/'; Path = (Join-Path $game 'ue4ss\Mods\UNBSEOBSE64Interop'); HasManifest = $false }
)
foreach ($ownedRoot in $ownedModRoots) {
    if (-not (Test-Path -LiteralPath $ownedRoot.Path)) { continue }
    Assert-UNBSENoReparsePath $ownedRoot.Path | Out-Null
    $allowed = @($pre.PackageManifest.artifacts | Where-Object {
        ([string]$_.relativePath).StartsWith($ownedRoot.Relative, [StringComparison]::Ordinal)
    } | ForEach-Object {
        [IO.Path]::GetFullPath((Join-Path $game ([string]$_.relativePath)))
    })
    if ($ownedRoot.HasManifest) {
        $allowed += [IO.Path]::GetFullPath((Join-Path $ownedRoot.Path 'unbse-mod-manifest.json'))
    }
    if ($legacyInteropDisablePresent -and
        $ownedRoot.Relative -ceq 'ue4ss/Mods/UNBSEOBSE64Interop/') {
        $allowed += [IO.Path]::GetFullPath($legacyInteropDisableMarker)
    }
    $unexpected = @(Get-ChildItem -LiteralPath $ownedRoot.Path -Recurse -File -Force | Where-Object {
        [IO.Path]::GetFullPath($_.FullName) -notin $allowed
    })
    if ($unexpected.Count -ne 0) {
        throw "Existing UNBSE base directory contains files this installer does not own: $($unexpected.FullName -join '; ')"
    }
}

$targets = @($pre.PackageManifest.artifacts | ForEach-Object { [string]$_.relativePath })
$targets += 'ue4ss/Mods/UNBSE/unbse-mod-manifest.json'
$old = @()
foreach ($relative in $targets) {
    $target = Join-Path $game $relative
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        Assert-UNBSENoReparsePath $target | Out-Null
        $old += [pscustomobject]@{
            RelativePath = $relative
            Hash = Get-UNBSEHash $target
            Existed = $true
            InstallReplacement = $true
        }
    }
    else {
        $old += [pscustomobject]@{
            RelativePath = $relative
            Hash = 'MISSING'
            Existed = $false
            InstallReplacement = $true
        }
    }
}
if ($legacyInteropDisablePresent) {
    $old += [pscustomobject]@{
        RelativePath = $legacyInteropDisableRelative
        Hash = Get-UNBSEHash $legacyInteropDisableMarker
        Existed = $true
        InstallReplacement = $false
    }
}
$oldSet = $old | Sort-Object RelativePath | ConvertTo-Json -Compress
$backupId = [Convert]::ToHexString(
    [Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($oldSet))).ToLowerInvariant()
$backup = Join-Path $game ".unbse-mod-backups\$backupId"
$stage = Join-Path $game ".unbse-mod-stage-$([guid]::NewGuid().ToString('N'))"

try {
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    foreach ($relative in $targets) {
        $source = if ($relative -eq 'ue4ss/Mods/UNBSE/unbse-mod-manifest.json') {
            $pre.PackageManifestPath
        }
        else {
            Join-Path $package $relative
        }
        $staged = Join-Path $stage $relative
        New-Item -ItemType Directory -Path (Split-Path $staged -Parent) -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $staged -Force
    }
    foreach ($entry in $old) {
        $target = Join-Path $game $entry.RelativePath
        if ($entry.Existed) {
            $backupFile = Join-Path $backup $entry.RelativePath
            New-Item -ItemType Directory -Path (Split-Path $backupFile -Parent) -Force | Out-Null
            Copy-Item -LiteralPath $target -Destination $backupFile -Force
        }
        if ($entry.InstallReplacement) {
            New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $stage $entry.RelativePath) -Destination $target -Force
        }
        elseif ($entry.Existed) {
            Remove-Item -LiteralPath $target -Force
        }
    }
    if ($ForcePostStageFailure) {
        throw 'Forced post-stage failure'
    }
    $post = Get-UNBSEUE4SSModAudit `
        -GameRoot $game `
        -PackageRoot $package `
        -FoundationSourceRoot $FoundationSourceRoot `
        -FoundationManifestPath $FoundationManifestPath
    if (-not $post.Success) {
        throw "Post-stage UNBSE audit failed: $($post.Errors -join '; ')"
    }
    [pscustomobject]@{
        Success = $true
        Changed = $true
        Status = 'installed'
        BackupId = $backupId
        GameRoot = $game
        DllSha256 = $pre.PackageManifest.artifacts[0].sha256
        Obse64InteropDllSha256 = $pre.PackageManifest.artifacts[2].sha256
        MigratedLegacyInteropDisableMarker = $legacyInteropDisablePresent
    } | ConvertTo-Json -Compress
}
catch {
    $failure = $_.Exception.Message
    foreach ($entry in $old) {
        $target = Join-Path $game $entry.RelativePath
        $backupFile = Join-Path $backup $entry.RelativePath
        if ($entry.Existed -and (Test-Path -LiteralPath $backupFile)) {
            Copy-Item -LiteralPath $backupFile -Destination $target -Force
        }
        elseif (-not $entry.Existed -and (Test-Path -LiteralPath $target)) {
            Remove-Item -LiteralPath $target -Force
        }
    }
    [pscustomobject]@{ Success = $false; Errors = @($failure); BackupId = $backupId } |
        ConvertTo-Json -Compress
    exit 1
}
finally {
    if (Test-Path -LiteralPath $stage) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
