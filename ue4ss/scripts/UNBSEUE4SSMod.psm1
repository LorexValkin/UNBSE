Set-StrictMode -Version Latest

Import-Module (Join-Path $PSScriptRoot 'UNBSEUE4SSFoundation.psm1') -Force

function Test-UNBSEModExactFile {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Sha256,
        [Parameter(Mandatory)][long]$Bytes,
        [Parameter(Mandatory)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return "$Label is missing: $Path"
    }
    try {
        Assert-UNBSENoReparsePath $Path | Out-Null
    }
    catch {
        return $_.Exception.Message
    }
    if ((Get-Item -LiteralPath $Path).Length -ne $Bytes) {
        return "$Label has an unexpected length: $Path"
    }
    if ((Get-UNBSEHash $Path) -ne $Sha256.ToUpperInvariant()) {
        return "$Label has an unexpected SHA-256: $Path"
    }
    return $null
}

function Get-UNBSEUE4SSModAudit {
    param(
        [Parameter(Mandatory)][string]$GameRoot,
        [Parameter(Mandatory)][string]$PackageRoot,
        [Parameter(Mandatory)][string]$FoundationSourceRoot,
        [string]$FoundationManifestPath = (Join-Path $PSScriptRoot '..\foundation-manifest.json')
    )

    $errors = [Collections.Generic.List[string]]::new()
    try {
        $game = Assert-UNBSENoReparsePath $GameRoot
        $package = Assert-UNBSENoReparsePath $PackageRoot
        $foundationManifestFile = Assert-UNBSENoReparsePath $FoundationManifestPath
        $foundationManifest = Get-Content -LiteralPath $foundationManifestFile -Raw | ConvertFrom-Json
        $packageManifestFile = Assert-UNBSENoReparsePath (Join-Path $package 'unbse-mod-manifest.json')
        $packageManifest = Get-Content -LiteralPath $packageManifestFile -Raw | ConvertFrom-Json
    }
    catch {
        $errors.Add($_.Exception.Message)
        return [pscustomobject]@{ Success = $false; Errors = @($errors) }
    }

    $foundationAudit = Get-UNBSEFoundationAudit `
        -GameRoot $game `
        -SourceRoot $FoundationSourceRoot `
        -ManifestPath $foundationManifestFile
    foreach ($errorText in $foundationAudit.Errors) {
        $errors.Add("Foundation: $errorText")
    }

    if ($packageManifest.schema -ne 'UNBSEUE4SSCorePackage' -or $packageManifest.schemaVersion -ne 1) {
        $errors.Add('Package manifest schema is unsupported.')
    }
    if ($packageManifest.foundationId -ne $foundationManifest.foundationId) {
        $errors.Add('Package foundationId does not match the pinned foundation.')
    }
    if ($packageManifest.upstreamCommit -ne $foundationManifest.upstream.commit) {
        $errors.Add('Package upstream commit does not match the pinned foundation.')
    }
    if ($packageManifest.name -ne $foundationManifest.unbseMod.name -or
        $packageManifest.version -ne $foundationManifest.unbseMod.version -or
        $packageManifest.buildConfiguration -ne $foundationManifest.unbseMod.buildConfiguration) {
        $errors.Add('Package UNBSE identity/version does not match the pinned foundation.')
    }

    $sourcePinLines = @(@($foundationManifest.unbseMod.sourceFiles) +
        @($foundationManifest.unbseMod.obse64Interop.sourceFiles) |
        Sort-Object relativePath | ForEach-Object {
            "$($_.relativePath)=$($_.sha256.ToUpperInvariant())"
        }) -join "`n"
    $expectedSourceAggregate = Get-UNBSEHashBytes (
        [Text.Encoding]::UTF8.GetBytes($sourcePinLines))
    if ([string]$packageManifest.sourceAggregateSha256 -cne $expectedSourceAggregate) {
        $errors.Add('Package source aggregate does not match the pinned core and OBSE64 sources.')
    }
    $expectedGeneratedState = $foundationManifest.unbseMod.buildGeneratedState.patternsleuthCargoLock
    $actualGeneratedState = $packageManifest.buildGeneratedState.patternsleuthCargoLock
    if ([string]$actualGeneratedState.baseWorkingTreeSha256 -cne
            [string]$expectedGeneratedState.baseWorkingTreeSha256 -or
        [string]$actualGeneratedState.resolvedWorkingTreeSha256 -cne
            [string]$expectedGeneratedState.resolvedWorkingTreeSha256 -or
        [bool]$actualGeneratedState.restoredAfterBuild -ne $true) {
        $errors.Add('Package build-generated state does not match the pinned foundation.')
    }
    $expectedPatches = @($foundationManifest.patchSet.patches | ForEach-Object {
        "$($_.relativePath)=$($_.sha256.ToUpperInvariant())"
    })
    $actualPatches = @($packageManifest.patches | ForEach-Object {
        "$($_.relativePath)=$($_.sha256.ToUpperInvariant())"
    })
    if (($actualPatches -join "`n") -cne ($expectedPatches -join "`n")) {
        $errors.Add('Package patch set does not match the pinned foundation.')
    }

    if ([string]$packageManifest.runtimeRole -cne 'capability-injector' -or
        [int]$packageManifest.addonHost.abiVersion -ne 1 -or
        [int]$packageManifest.addonHost.messagingAbiVersion -ne 1 -or
        [int]$packageManifest.addonHost.runtimeInfoAbiVersion -ne 1 -or
        [int]$packageManifest.addonHost.relocationAbiVersion -ne 1 -or
        [string]$packageManifest.addonHost.compatibilityPolicy -cne 'report-and-attempt' -or
        @($packageManifest.addonHost.bundledAddons).Count -ne 0) {
        $errors.Add('Core package add-on host boundary is unsupported.')
    }
    if ((@($packageManifest.requiredExports) -join "`n") -cne
        (@($foundationManifest.unbseMod.requiredExports) -join "`n")) {
        $errors.Add('Core package required export contract does not match the pinned foundation.')
    }
    $loader = $foundationManifest.unbseMod.loader
    if ([string]$packageManifest.launcher.name -cne [string]$loader.name -or
        [string]$packageManifest.launcher.packageExecutable -cne
            [string]$loader.packageExecutable -or
        [string]$packageManifest.launcher.supportedDistribution -cne
            [string]$loader.supportedDistribution -or
        [string]$packageManifest.launcher.supportedRuntimeVersion -cne
            [string]$loader.supportedRuntimeVersion -or
        [string]$packageManifest.launcher.injectionPolicy -cne
            [string]$loader.injectionPolicy) {
        $errors.Add('UNBSE launcher contract does not match the pinned foundation.')
    }
    $interop = $foundationManifest.unbseMod.obse64Interop
    $baseComponents = @($packageManifest.baseComponents)
    if ($baseComponents.Count -ne 1 -or
        [string]$baseComponents[0].componentId -cne 'obse64-interop' -or
        [string]$baseComponents[0].name -cne [string]$interop.name -or
        [string]$baseComponents[0].version -cne [string]$interop.version -or
        [string]$baseComponents[0].compatibilityPolicy -cne 'report-and-attempt' -or
        [string]$baseComponents[0].compatibilityClaim -cne 'unverified-attempt' -or
        (@($baseComponents[0].requiredHostCapabilities) -join "`n") -cne
            (@($interop.requiredHostCapabilities) -join "`n") -or
        (@($baseComponents[0].declaredEffects) -join "`n") -cne
            (@($interop.declaredEffects) -join "`n") -or
        [string]$baseComponents[0].pluginDirectory -cne 'OBSE/Plugins' -or
        [string]$baseComponents[0].provenance.abiReferenceRepository -cne
            [string]$interop.provenance.abiReferenceRepository -or
        [string]$baseComponents[0].provenance.abiReferenceCommit -cne
            [string]$interop.provenance.abiReferenceCommit -or
        [string]$baseComponents[0].provenance.abiReferenceFile -cne
            [string]$interop.provenance.abiReferenceFile -or
        [string]$baseComponents[0].provenance.implementationPolicy -cne
            [string]$interop.provenance.implementationPolicy) {
        $errors.Add('OBSE64 base component contract is unsupported.')
    }

    $expectedArtifacts = @($packageManifest.artifacts)
    $expectedRelativePaths = @(
        [string]$foundationManifest.unbseMod.packageDll,
        'ue4ss/Mods/UNBSE/enabled.txt',
        'ue4ss/Mods/UNBSE/Scripts/main.lua',
        [string]$interop.packageDll,
        [string]$interop.enabledFile,
        'ue4ss/Mods/UNBSEOBSE64Interop/Scripts/main.lua',
        [string]$loader.packageExecutable,
        'ue4ss/UE4SS.dll',
        'ue4ss/Mods/UNBSE/sdk/UNBSEAddonHostV1.h',
        'ue4ss/Mods/UNBSE/sdk/UNBSEScriptServiceV1.h',
        'ue4ss/Mods/UNBSE/sdk/UNBSEMessagingV1.h',
        'ue4ss/Mods/UNBSE/sdk/UNBSERuntimeInfoV1.h',
        'ue4ss/Mods/UNBSE/sdk/UNBSERelocationV1.h',
        'ue4ss/Mods/UNBSE/sdk/README.md'
    )
    if ($expectedArtifacts.Count -ne $expectedRelativePaths.Count -or
        $expectedArtifacts[0].relativePath -ne $foundationManifest.unbseMod.packageDll -or
        (@($expectedArtifacts | ForEach-Object { [string]$_.relativePath }) -join "`n") -cne
            ($expectedRelativePaths -join "`n")) {
        $errors.Add('Package artifact list or order is unsupported.')
    }
    $hostArtifact = @($expectedArtifacts | Where-Object {
        [string]$_.relativePath -ceq 'ue4ss/UE4SS.dll'
    })
    if ($hostArtifact.Count -ne 1 -or
        [string]$packageManifest.patchedFoundation.relativePath -cne 'ue4ss/UE4SS.dll' -or
        [string]$packageManifest.patchedFoundation.sha256 -cne [string]$hostArtifact[0].sha256 -or
        [long]$packageManifest.patchedFoundation.bytes -ne [long]$hostArtifact[0].bytes -or
        (@($packageManifest.patchedFoundation.requiredCapabilityMarkers) -join "`n") -cne
            ("UE4SS.CppModLifecycle`n" +
             '[UNBSE] Legacy Lua ExecuteConsoleCommand call adapted')) {
        $errors.Add('Package patched UE4SS foundation contract is unsupported.')
    }
    foreach ($artifact in $expectedArtifacts) {
        $relative = [string]$artifact.relativePath
        $ownedPath = $relative.StartsWith('ue4ss/Mods/UNBSE/', [StringComparison]::Ordinal) -or
            $relative.StartsWith('ue4ss/Mods/UNBSEOBSE64Interop/', [StringComparison]::Ordinal) -or
            $relative -ceq [string]$loader.packageExecutable -or
            $relative -ceq 'ue4ss/UE4SS.dll'
        if ([IO.Path]::IsPathRooted($relative) -or $relative.Contains('..') -or -not $ownedPath) {
            $errors.Add("Package artifact path is unsafe: $relative")
            continue
        }
        $packageFile = Join-Path $package $relative
        $packageError = Test-UNBSEModExactFile `
            -Path $packageFile `
            -Sha256 ([string]$artifact.sha256) `
            -Bytes ([long]$artifact.bytes) `
            -Label "Package artifact $relative"
        if ($packageError) {
            $errors.Add($packageError)
        }
    }
    $installedErrors = [Collections.Generic.List[string]]::new()
    foreach ($artifact in $expectedArtifacts) {
        $relative = [string]$artifact.relativePath
        $installedFile = Join-Path $game $relative
        $installedError = Test-UNBSEModExactFile `
            -Path $installedFile `
            -Sha256 ([string]$artifact.sha256) `
            -Bytes ([long]$artifact.bytes) `
            -Label "Installed artifact $relative"
        if ($installedError) {
            $installedErrors.Add($installedError)
        }
    }
    $installedManifest = Join-Path $game 'ue4ss\Mods\UNBSE\unbse-mod-manifest.json'
    if (-not (Test-Path -LiteralPath $installedManifest -PathType Leaf)) {
        $installedErrors.Add('Installed UNBSE package manifest is missing or differs from the package.')
    }
    elseif ((Get-UNBSEHash $installedManifest) -ne (Get-UNBSEHash $packageManifestFile)) {
        $installedErrors.Add('Installed UNBSE package manifest is missing or differs from the package.')
    }

    [pscustomobject]@{
        Success = ($errors.Count -eq 0 -and $installedErrors.Count -eq 0)
        FoundationSuccess = $foundationAudit.Success
        PackageSuccess = ($errors.Count -eq 0)
        InstalledSuccess = ($installedErrors.Count -eq 0)
        Errors = @($errors) + @($installedErrors)
        PackageErrors = @($errors)
        InstalledErrors = @($installedErrors)
        GameRoot = $game
        PackageRoot = $package
        PackageManifest = $packageManifest
        PackageManifestPath = $packageManifestFile
    }
}

Export-ModuleMember -Function Get-UNBSEUE4SSModAudit
