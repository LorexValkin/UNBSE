[CmdletBinding()]
param(
    [string]$ManifestPath = (Join-Path $PSScriptRoot '..\foundation-manifest.json'),
    [string]$SourceRoot = (Join-Path $PSScriptRoot '..\..\out\ue4ss-source'),
    [string]$BuildRoot = (Join-Path $PSScriptRoot '..\..\out\ue4ss-build'),
    [string]$InteropBuildRoot = (Join-Path $PSScriptRoot '..\..\out\ue4ss-build-obse64interop'),
    [string]$LoaderBuildRoot = (Join-Path $PSScriptRoot '..\..\out\unbse-loader-build'),
    [string]$PackageRoot = (Join-Path $PSScriptRoot '..\..\out\ue4ss-package'),
    [ValidateRange(1, 64)][int]$Parallel = 8
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Some automation hosts supply both PATH and Path in the process block. Legacy
# MSBuild treats those case variants as duplicate dictionary keys when it starts
# cl.exe, so normalize the process entry before invoking the toolchain.
$normalizedProcessPath = [Environment]::GetEnvironmentVariable('Path', 'Process')
[Environment]::SetEnvironmentVariable('PATH', $null, 'Process')
[Environment]::SetEnvironmentVariable('Path', $null, 'Process')
[Environment]::SetEnvironmentVariable('Path', $normalizedProcessPath, 'Process')

Import-Module (Join-Path $PSScriptRoot 'UNBSEUE4SSFoundation.psm1') -Force

function Get-UNBSEBuildCanonicalPath {
    param([Parameter(Mandatory)][string]$Path, [switch]$MustExist)
    $full = [IO.Path]::GetFullPath($Path)
    if ($MustExist -and -not (Test-Path -LiteralPath $full)) {
        throw "Path does not exist: $full"
    }
    return $full.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
}

function Get-UNBSECanonicalTextHash {
    param([Parameter(Mandatory)][string]$Path)

    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    $text = $utf8.GetString([IO.File]::ReadAllBytes($Path))
    $canonicalText = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    return Get-UNBSEHashBytes ($utf8.GetBytes($canonicalText))
}

function Invoke-UNBSENative {
    param(
        [Parameter(Mandatory)][string]$Command,
        [Parameter(Mandatory)][string[]]$Arguments,
        [switch]$Capture
    )

    if ($Capture) {
        $output = @(& $Command @Arguments 2>&1)
        if ($LASTEXITCODE -ne 0) {
            throw "$Command failed with exit code $LASTEXITCODE`: $($output -join [Environment]::NewLine)"
        }
        return ($output -join [Environment]::NewLine).Trim()
    }

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}

function Get-UNBSEGitFilteredBytes {
    param(
        [Parameter(Mandatory)][string]$Repository,
        [Parameter(Mandatory)][string]$RelativePath
    )

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = 'git'
    $startInfo.WorkingDirectory = $Repository
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.Arguments = "cat-file --filters --path=`"$RelativePath`" `"HEAD:$RelativePath`""
    $process = [Diagnostics.Process]::Start($startInfo)
    $stream = [IO.MemoryStream]::new()
    $process.StandardOutput.BaseStream.CopyTo($stream)
    $errorText = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "Unable to materialize pinned Git bytes for $RelativePath`: $errorText"
    }
    return $stream.ToArray()
}

foreach ($tool in @('git', 'cmake', 'cargo')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Required build tool is unavailable: $tool"
    }
}
$gitExecutable = (Get-Command git -ErrorAction Stop).Source
$gitInstallRoot = Split-Path -Path (Split-Path -Path $gitExecutable -Parent) -Parent
foreach ($gitToolDirectory in @(
        (Join-Path $gitInstallRoot 'usr\bin'),
        (Join-Path $gitInstallRoot 'mingw64\bin'))) {
    if (Test-Path -LiteralPath $gitToolDirectory -PathType Container) {
        $env:PATH = "$gitToolDirectory$([IO.Path]::PathSeparator)$env:PATH"
    }
}

$manifestFile = Assert-UNBSENoReparsePath (Get-UNBSEBuildCanonicalPath $ManifestPath -MustExist)
$manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
$repositoryRoot = Get-UNBSEBuildCanonicalPath (Join-Path $PSScriptRoot '..\..') -MustExist
$source = Assert-UNBSENoReparsePath $SourceRoot -AllowMissingLeaf
$build = Assert-UNBSENoReparsePath $BuildRoot -AllowMissingLeaf
$interopBuild = Assert-UNBSENoReparsePath $InteropBuildRoot -AllowMissingLeaf
$loaderBuild = Assert-UNBSENoReparsePath $LoaderBuildRoot -AllowMissingLeaf
$package = Assert-UNBSENoReparsePath $PackageRoot -AllowMissingLeaf
$modSource = Assert-UNBSENoReparsePath (Join-Path $PSScriptRoot '..\mod\UNBSE')
$interop = $manifest.unbseMod.obse64Interop
$loader = $manifest.unbseMod.loader
if ([string]$interop.name -cne 'UNBSEOBSE64Interop' -or
    [string]$interop.packageDll -cne 'ue4ss/Mods/UNBSEOBSE64Interop/dlls/main.dll' -or
    [string]$interop.enabledFile -cne 'ue4ss/Mods/UNBSEOBSE64Interop/enabled.txt') {
    throw 'The authenticated OBSE64 base-component package boundary has drifted.'
}
$interopSource = Assert-UNBSENoReparsePath (Join-Path $repositoryRoot $interop.sourceDirectory)
if ([string]$loader.name -cne 'UNBSELoader' -or
    [string]$loader.target -cne 'UNBSELoader' -or
    [string]$loader.packageExecutable -cne 'UNBSELoader.exe') {
    throw 'The authenticated UNBSE launcher package boundary has drifted.'
}
$loaderSource = Assert-UNBSENoReparsePath (Join-Path $repositoryRoot $loader.sourceDirectory)
$patches = @()
foreach ($patchRecord in $manifest.patchSet.patches) {
    $patchPath = Assert-UNBSENoReparsePath (Join-Path $repositoryRoot $patchRecord.relativePath)
    if ((Get-UNBSECanonicalTextHash $patchPath) -ne $patchRecord.sha256.ToUpperInvariant()) {
        throw "UE4SS patch SHA-256 differs from the foundation manifest: $patchPath"
    }
    $applyRoot = if ($patchRecord.PSObject.Properties.Name -contains 'applyRoot') {
        [string]$patchRecord.applyRoot
    } else { '' }
    if ($applyRoot -and $applyRoot -notin @([string[]]$manifest.upstream.submodules.path)) {
        throw "UE4SS patch applyRoot is not a pinned submodule: $applyRoot"
    }
    $expectedChangedPaths = if ($patchRecord.PSObject.Properties.Name -contains 'expectedChangedPaths') {
        @($patchRecord.expectedChangedPaths | ForEach-Object { [string]$_ })
    } else { @() }
    if (@($expectedChangedPaths).Count -eq 0) {
        throw "Patch has no exact expectedChangedPaths: $($patchRecord.relativePath)"
    }
    $patches += [pscustomobject]@{
        Path = $patchPath
        ApplyRoot = $applyRoot
        ExpectedChangedPaths = $expectedChangedPaths
    }
}
foreach ($sourceFile in $manifest.unbseMod.sourceFiles) {
    $path = Assert-UNBSENoReparsePath (Join-Path $repositoryRoot $sourceFile.relativePath)
    if ((Get-UNBSECanonicalTextHash $path) -ne $sourceFile.sha256.ToUpperInvariant()) {
        throw "UNBSE mod source SHA-256 differs from the foundation manifest: $($sourceFile.relativePath)"
    }
}

if (-not (Test-Path -LiteralPath $source)) {
    New-Item -ItemType Directory -Path $source | Out-Null
}
if (-not (Test-Path -LiteralPath (Join-Path $source '.git'))) {
    if (@(Get-ChildItem -LiteralPath $source -Force).Count -ne 0) {
        throw "SourceRoot exists but is neither empty nor a Git worktree: $source"
    }
    Invoke-UNBSENative git @('-C', $source, 'init')
    Invoke-UNBSENative git @('-C', $source, 'remote', 'add', 'origin', [string]$manifest.upstream.repository)
    Invoke-UNBSENative git @('-C', $source, 'fetch', '--depth', '1', 'origin', [string]$manifest.upstream.commit)
    Invoke-UNBSENative git @('-C', $source, 'checkout', '--detach', [string]$manifest.upstream.commit)
}

$head = Invoke-UNBSENative git @('-C', $source, 'rev-parse', 'HEAD') -Capture
if ($head -ne [string]$manifest.upstream.commit) {
    throw "UE4SS source is at $head, expected $($manifest.upstream.commit). Use a fresh SourceRoot."
}

# The pinned upstream records SSH submodule URLs. HTTPS overrides make the
# materialization non-interactive while preserving the exact recorded commits.
Invoke-UNBSENative git @('-C', $source, 'config', 'submodule.deps/first/Unreal.url', 'https://github.com/Re-UE4SS/UEPseudo.git')
Invoke-UNBSENative git @('-C', $source, 'config', 'submodule.deps/first/patternsleuth.url', 'https://github.com/trumank/patternsleuth.git')
$missingSubmodule = @($manifest.upstream.submodules | Where-Object {
    -not (Test-Path -LiteralPath (Join-Path (Join-Path $source $_.path) '.git'))
}).Count -ne 0
if ($missingSubmodule) {
    Invoke-UNBSENative git @('-C', $source, 'submodule', 'update', '--init', '--recursive', '--depth', '1')
}
foreach ($submodule in $manifest.upstream.submodules) {
    $submoduleRoot = Assert-UNBSENoReparsePath (Join-Path $source $submodule.path)
    $submoduleHead = Invoke-UNBSENative git @('-C', $submoduleRoot, 'rev-parse', 'HEAD') -Capture
    if ($submoduleHead -ne [string]$submodule.commit) {
        throw "Submodule $($submodule.path) is at $submoduleHead, expected $($submodule.commit)."
    }
}

foreach ($patch in $patches) {
    $patchRoot = if ($patch.ApplyRoot) {
        Assert-UNBSENoReparsePath (Join-Path $source $patch.ApplyRoot)
    } else { $source }
    # Prefer the exact reverse check: insertion-only patches can remain
    # forward-applicable after one application if their anchor follows the
    # inserted block. Recognizing the applied state first keeps reruns
    # idempotent instead of duplicating the insertion.
    $savedErrorPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & git -C $patchRoot apply --reverse --check --unidiff-zero $patch.Path 2>$null
    $reverseCheckExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorPreference
    if ($reverseCheckExitCode -eq 0) {
        continue
    }
    $ErrorActionPreference = 'Continue'
    & git -C $patchRoot apply --check --unidiff-zero $patch.Path 2>$null
    $forwardCheckExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorPreference
    if ($forwardCheckExitCode -ne 0) {
        throw "A UNBSE patch is neither cleanly applicable nor already applied: $($patch.Path)"
    }
    Invoke-UNBSENative git @('-C', $patchRoot, 'apply', '--unidiff-zero', $patch.Path)
}

$cargoLockRecord = $manifest.unbseMod.buildGeneratedState.patternsleuthCargoLock
$cargoLock = Assert-UNBSENoReparsePath (Join-Path $source $cargoLockRecord.relativePath)
$baseCargoLockBytes = Get-UNBSEGitFilteredBytes $source $cargoLockRecord.relativePath
$baseCargoLockHash = Get-UNBSEHashBytes $baseCargoLockBytes
if ($baseCargoLockHash -ne $cargoLockRecord.baseWorkingTreeSha256.ToUpperInvariant()) {
    throw "Pinned Cargo.lock filtered bytes differ from the foundation manifest: $baseCargoLockHash"
}
$currentCargoLockHash = Get-UNBSEHash $cargoLock
if ($currentCargoLockHash -notin @(
        $cargoLockRecord.baseWorkingTreeSha256.ToUpperInvariant(),
        $cargoLockRecord.resolvedWorkingTreeSha256.ToUpperInvariant())) {
    throw "Cargo.lock is neither the pinned base nor the one expected build-generated resolution: $currentCargoLockHash"
}
# Normalize an interrupted prior build before checking the source patch set.
[IO.File]::WriteAllBytes($cargoLock, $baseCargoLockBytes)

$trackedStatus = @(& git -C $source status --porcelain --untracked-files=no --ignore-submodules=all)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to inspect the patched UE4SS worktree.'
}
$expectedStatus = @($patches | Where-Object { -not $_.ApplyRoot } |
    ForEach-Object { $_.ExpectedChangedPaths } | Sort-Object -Unique |
    ForEach-Object { " M $_" })
if (@(Compare-Object $expectedStatus $trackedStatus).Count -ne 0) {
    throw "Pinned UE4SS worktree has unexpected tracked changes: $($trackedStatus -join '; ')"
}
foreach ($submodule in $manifest.upstream.submodules) {
    $submodulePatches = @($patches | Where-Object { $_.ApplyRoot -ceq [string]$submodule.path })
    if (@($submodulePatches).Count -eq 0) { continue }
    $submoduleRoot = Assert-UNBSENoReparsePath (Join-Path $source $submodule.path)
    $submoduleStatus = @(& git -C $submoduleRoot status --porcelain --untracked-files=no)
    if ($LASTEXITCODE -ne 0) { throw "Unable to inspect patched submodule: $($submodule.path)" }
    $expectedSubmoduleStatus = @($submodulePatches.ExpectedChangedPaths | Sort-Object -Unique |
        ForEach-Object { " M $_" })
    if (@(Compare-Object $expectedSubmoduleStatus $submoduleStatus).Count -ne 0) {
        throw "Pinned submodule $($submodule.path) has unexpected tracked changes: $($submoduleStatus -join '; ')"
    }
}

New-Item -ItemType Directory -Path $build -Force | Out-Null
try {
    Invoke-UNBSENative cmake @(
        '-S', $source,
        '-B', $build,
        '-G', 'Visual Studio 17 2022',
        '-A', 'x64',
        "-DUNBSE_MOD_SOURCE_DIR=$($modSource.Replace('\', '/'))"
    )
    Invoke-UNBSENative cmake @(
        '--build', $build,
        '--config', [string]$manifest.unbseMod.buildConfiguration,
        '--target', [string]$manifest.unbseMod.target,
        '--parallel', [string]$Parallel
    )
    New-Item -ItemType Directory -Path $interopBuild -Force | Out-Null
    Invoke-UNBSENative cmake @(
        '-S', $source,
        '-B', $interopBuild,
        '-G', 'Visual Studio 17 2022',
        '-A', 'x64',
        "-DUNBSE_MOD_SOURCE_DIR=$($interopSource.Replace('\', '/'))"
    )
    Invoke-UNBSENative cmake @(
        '--build', $interopBuild,
        '--config', [string]$interop.buildConfiguration,
        '--target', [string]$interop.target,
        '--parallel', [string]$Parallel
    )
    New-Item -ItemType Directory -Path $loaderBuild -Force | Out-Null
    Invoke-UNBSENative cmake @(
        '-S', $loaderSource,
        '-B', $loaderBuild,
        '-G', 'Visual Studio 17 2022',
        '-A', 'x64'
    )
    Invoke-UNBSENative cmake @(
        '--build', $loaderBuild,
        '--config', [string]$loader.buildConfiguration,
        '--target', [string]$loader.target,
        '--parallel', [string]$Parallel
    )
    $resolvedCargoLockHash = Get-UNBSEHash $cargoLock
    if ($resolvedCargoLockHash -ne $cargoLockRecord.resolvedWorkingTreeSha256.ToUpperInvariant()) {
        throw "Cargo produced an unrecognized dependency resolution: $resolvedCargoLockHash"
    }
}
finally {
    # Cargo 1.95 removes stale, unused packages from this pinned lock during the
    # build. Verify that exact mutation above, then leave the source patch-clean.
    [IO.File]::WriteAllBytes($cargoLock, $baseCargoLockBytes)
    if ((Get-UNBSEHash $cargoLock) -ne $cargoLockRecord.baseWorkingTreeSha256.ToUpperInvariant()) {
        throw 'Failed to restore the pinned Cargo.lock working-tree bytes.'
    }
}

$artifact = Join-Path $build "$($manifest.unbseMod.buildConfiguration)\bin\main.dll"
$artifact = Assert-UNBSENoReparsePath (Get-UNBSEBuildCanonicalPath $artifact -MustExist)
$dllHash = Get-UNBSEHash $artifact
$dllBytes = (Get-Item -LiteralPath $artifact).Length
$interopArtifact = Join-Path $interopBuild "$($interop.buildConfiguration)\bin\main.dll"
$interopArtifact = Assert-UNBSENoReparsePath `
    (Get-UNBSEBuildCanonicalPath $interopArtifact -MustExist)
$interopDllHash = Get-UNBSEHash $interopArtifact
$interopDllBytes = (Get-Item -LiteralPath $interopArtifact).Length
$loaderArtifact = Join-Path $loaderBuild "$($loader.buildConfiguration)\$($loader.packageExecutable)"
$loaderArtifact = Assert-UNBSENoReparsePath `
    (Get-UNBSEBuildCanonicalPath $loaderArtifact -MustExist)
$loaderHash = Get-UNBSEHash $loaderArtifact
$loaderBytes = (Get-Item -LiteralPath $loaderArtifact).Length
$packageDll = Join-Path $package $manifest.unbseMod.packageDll
New-Item -ItemType Directory -Path (Split-Path $packageDll -Parent) -Force | Out-Null
Copy-Item -LiteralPath $artifact -Destination $packageDll -Force
$enabled = Join-Path $package 'ue4ss\Mods\UNBSE\enabled.txt'
[IO.File]::WriteAllText($enabled, '', [Text.UTF8Encoding]::new($false))
$coreLuaStub = Join-Path $package 'ue4ss\Mods\UNBSE\Scripts\main.lua'
New-Item -ItemType Directory -Path (Split-Path $coreLuaStub -Parent) -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $modSource 'Scripts\main.lua') `
    -Destination $coreLuaStub -Force
$interopPackageDll = Join-Path $package $interop.packageDll
New-Item -ItemType Directory -Path (Split-Path $interopPackageDll -Parent) -Force | Out-Null
Copy-Item -LiteralPath $interopArtifact -Destination $interopPackageDll -Force
$interopEnabled = Join-Path $package $interop.enabledFile
New-Item -ItemType Directory -Path (Split-Path $interopEnabled -Parent) -Force | Out-Null
[IO.File]::WriteAllText($interopEnabled, '', [Text.UTF8Encoding]::new($false))
$interopLuaStub = Join-Path $package 'ue4ss\Mods\UNBSEOBSE64Interop\Scripts\main.lua'
New-Item -ItemType Directory -Path (Split-Path $interopLuaStub -Parent) -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $interopSource 'Scripts\main.lua') `
    -Destination $interopLuaStub -Force
$loaderPackageExecutable = Join-Path $package $loader.packageExecutable
Copy-Item -LiteralPath $loaderArtifact -Destination $loaderPackageExecutable -Force
foreach ($sourceFile in $interop.sourceFiles) {
    $path = Assert-UNBSENoReparsePath (Join-Path $repositoryRoot $sourceFile.relativePath)
    if ((Get-UNBSECanonicalTextHash $path) -ne $sourceFile.sha256.ToUpperInvariant()) {
        throw "OBSE64 base component source SHA-256 differs from the foundation manifest: $($sourceFile.relativePath)"
    }
}
$sdkSources = @(
    [ordered]@{
        source = Assert-UNBSENoReparsePath (Join-Path $repositoryRoot 'include\UNBSEAddonHostV1.h')
        relativePath = 'ue4ss/Mods/UNBSE/sdk/UNBSEAddonHostV1.h'
    },
    [ordered]@{
        source = Assert-UNBSENoReparsePath (Join-Path $repositoryRoot 'include\UNBSEScriptServiceV1.h')
        relativePath = 'ue4ss/Mods/UNBSE/sdk/UNBSEScriptServiceV1.h'
    },
    [ordered]@{
        source = Assert-UNBSENoReparsePath (Join-Path $repositoryRoot 'include\UNBSEMessagingV1.h')
        relativePath = 'ue4ss/Mods/UNBSE/sdk/UNBSEMessagingV1.h'
    },
    [ordered]@{
        source = Assert-UNBSENoReparsePath (Join-Path $repositoryRoot 'include\UNBSERuntimeInfoV1.h')
        relativePath = 'ue4ss/Mods/UNBSE/sdk/UNBSERuntimeInfoV1.h'
    },
    [ordered]@{
        source = Assert-UNBSENoReparsePath (Join-Path $repositoryRoot 'include\UNBSERelocationV1.h')
        relativePath = 'ue4ss/Mods/UNBSE/sdk/UNBSERelocationV1.h'
    },
    [ordered]@{
        source = Assert-UNBSENoReparsePath (Join-Path $repositoryRoot 'ue4ss\addons\README.md')
        relativePath = 'ue4ss/Mods/UNBSE/sdk/README.md'
    }
)
$sdkArtifacts = @()
foreach ($sdkSource in $sdkSources) {
    $sdkTarget = Join-Path $package $sdkSource.relativePath
    New-Item -ItemType Directory -Path (Split-Path $sdkTarget -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $sdkSource.source -Destination $sdkTarget -Force
    $sdkArtifacts += [ordered]@{
        relativePath = [string]$sdkSource.relativePath
        sha256 = Get-UNBSEHash $sdkTarget
        bytes = [long](Get-Item -LiteralPath $sdkTarget).Length
    }
}

$sourcePins = @(@($manifest.unbseMod.sourceFiles) + @($interop.sourceFiles) |
    Sort-Object relativePath | ForEach-Object {
    "$($_.relativePath)=$($_.sha256.ToUpperInvariant())"
}) -join "`n"
$sourceAggregate = Get-UNBSEHashBytes ([Text.Encoding]::UTF8.GetBytes($sourcePins))
$packageManifest = [ordered]@{
    schema = 'UNBSEUE4SSCorePackage'
    schemaVersion = 1
    name = [string]$manifest.unbseMod.name
    version = [string]$manifest.unbseMod.version
    foundationId = [string]$manifest.foundationId
    upstreamCommit = [string]$manifest.upstream.commit
    buildConfiguration = [string]$manifest.unbseMod.buildConfiguration
    sourceAggregateSha256 = $sourceAggregate
    buildGeneratedState = [ordered]@{
        patternsleuthCargoLock = [ordered]@{
            baseWorkingTreeSha256 = [string]$cargoLockRecord.baseWorkingTreeSha256
            resolvedWorkingTreeSha256 = [string]$cargoLockRecord.resolvedWorkingTreeSha256
            restoredAfterBuild = $true
        }
    }
    patches = @($manifest.patchSet.patches | ForEach-Object {
        [ordered]@{
            relativePath = [string]$_.relativePath
            sha256 = [string]$_.sha256
        }
    })
    runtimeRole = 'capability-injector'
    requiredExports = @($manifest.unbseMod.requiredExports)
    launcher = [ordered]@{
        name = [string]$loader.name
        packageExecutable = [string]$loader.packageExecutable
        supportedDistribution = [string]$loader.supportedDistribution
        supportedRuntimeVersion = [string]$loader.supportedRuntimeVersion
        injectionPolicy = [string]$loader.injectionPolicy
    }
    addonHost = [ordered]@{
        abiVersion = 1
        messagingAbiVersion = 1
        runtimeInfoAbiVersion = 1
        relocationAbiVersion = 1
        compatibilityPolicy = 'report-and-attempt'
        bundledAddons = @()
    }
    baseComponents = @(
        [ordered]@{
            componentId = 'obse64-interop'
            name = [string]$interop.name
            version = [string]$interop.version
            compatibilityPolicy = [string]$interop.compatibilityPolicy
            compatibilityClaim = [string]$interop.compatibilityClaim
            requiredHostCapabilities = @($interop.requiredHostCapabilities)
            declaredEffects = @($interop.declaredEffects)
            pluginDirectory = [string]$interop.pluginDirectory
            provenance = $interop.provenance
        }
    )
    artifacts = @(
        [ordered]@{
            relativePath = [string]$manifest.unbseMod.packageDll
            sha256 = $dllHash
            bytes = $dllBytes
        },
        [ordered]@{
            relativePath = 'ue4ss/Mods/UNBSE/enabled.txt'
            sha256 = 'E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855'
            bytes = 0
        },
        [ordered]@{
            relativePath = 'ue4ss/Mods/UNBSE/Scripts/main.lua'
            sha256 = Get-UNBSEHash $coreLuaStub
            bytes = [long](Get-Item -LiteralPath $coreLuaStub).Length
        },
        [ordered]@{
            relativePath = [string]$interop.packageDll
            sha256 = $interopDllHash
            bytes = $interopDllBytes
        },
        [ordered]@{
            relativePath = [string]$interop.enabledFile
            sha256 = 'E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855'
            bytes = 0
        },
        [ordered]@{
            relativePath = 'ue4ss/Mods/UNBSEOBSE64Interop/Scripts/main.lua'
            sha256 = Get-UNBSEHash $interopLuaStub
            bytes = [long](Get-Item -LiteralPath $interopLuaStub).Length
        },
        [ordered]@{
            relativePath = [string]$loader.packageExecutable
            sha256 = $loaderHash
            bytes = $loaderBytes
        }
    ) + $sdkArtifacts
}
$packageManifestPath = Join-Path $package 'unbse-mod-manifest.json'
[IO.File]::WriteAllText(
    $packageManifestPath,
    ($packageManifest | ConvertTo-Json -Depth 8),
    [Text.UTF8Encoding]::new($false))

[pscustomobject]@{
    Success = $true
    FoundationId = $manifest.foundationId
    SourceRoot = $source
    BuildRoot = $build
    InteropBuildRoot = $interopBuild
    LoaderBuildRoot = $loaderBuild
    PackageRoot = $package
    DllSha256 = $dllHash
    DllBytes = $dllBytes
    Obse64InteropDllSha256 = $interopDllHash
    Obse64InteropDllBytes = $interopDllBytes
    LoaderSha256 = $loaderHash
    LoaderBytes = $loaderBytes
    PackageManifest = $packageManifestPath
} | ConvertTo-Json -Compress
