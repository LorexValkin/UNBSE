[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$ManifestPath = (Join-Path $PSScriptRoot '..\foundation-manifest.json'),
    [switch]$Check
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Fail([string]$Message) { throw "UNBSE core-manifest boundary: $Message" }

function Get-Sha256([string]$Path) {
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    $text = $utf8.GetString([IO.File]::ReadAllBytes($Path))
    $canonicalText = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    $stream = [IO.MemoryStream]::new($utf8.GetBytes($canonicalText), $false)
    try {
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try {
            return ([BitConverter]::ToString($sha256.ComputeHash($stream))).Replace('-', '')
        }
        finally {
            $sha256.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Get-CanonicalFile([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { Fail "required file is missing: $full" }
    $probe = $full
    while ($true) {
        $item = Get-Item -LiteralPath $probe -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Fail "reparse target is not permitted: $probe"
        }
        $parent = Split-Path -Path $probe -Parent
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $probe) { break }
        $probe = $parent
    }
    return $full
}

$manifestFull = Get-CanonicalFile $ManifestPath
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$repositoryRoot = $repositoryRoot.TrimEnd(
    [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
$coreVersion = '0.12.0'
$foundationId = 'ue4ss-3.0.1-beta0-68dd45cb-unbse-patchset-v1-mod-0.12.0'
$coreSourcePaths = @(
    'include/UNBSEAddonHostV1.h',
    'include/UNBSEMessagingV1.h',
    'include/UNBSERelocationV1.h',
    'include/UNBSERuntimeInfoV1.h',
    'include/UNBSEScriptServiceV1.h',
    'ue4ss/addons/README.md',
    'ue4ss/loader/CMakeLists.txt',
    'ue4ss/loader/include/UNBSEAssetPreflight.hpp',
    'ue4ss/loader/src/main.cpp',
    'ue4ss/loader/src/UNBSEAssetPreflight.cpp',
    'ue4ss/mod/UNBSE/CMakeLists.txt',
    'ue4ss/mod/UNBSE/Scripts/main.lua',
    'ue4ss/mod/UNBSE/include/AddonRegistry.hpp',
    'ue4ss/mod/UNBSE/include/CoreLuaBinding.hpp',
    'ue4ss/mod/UNBSE/include/ScriptServiceRegistry.hpp',
    'ue4ss/mod/UNBSE/include/UNBSECoreMod.hpp',
    'ue4ss/mod/UNBSE/src/AddonRegistry.cpp',
    'ue4ss/mod/UNBSE/src/CoreLuaBinding.cpp',
    'ue4ss/mod/UNBSE/src/ScriptServiceRegistry.cpp',
    'ue4ss/mod/UNBSE/src/UNBSECoreMod.cpp',
    'ue4ss/mod/UNBSE/src/core_dllmain.cpp',
    'ue4ss/scripts/Build-UNBSEUE4SSMod.ps1',
    'ue4ss/scripts/Install-UNBSEUE4SSMod.ps1',
    'ue4ss/scripts/Package-UNBSERelease.ps1',
    'ue4ss/scripts/Set-UNBSECoreManifestBoundary.ps1',
    'ue4ss/scripts/UNBSEUE4SSFoundation.psm1',
    'ue4ss/scripts/UNBSEUE4SSMod.psm1',
    'ue4ss/scripts/Update-UNBSESourceFilePins.ps1'
)
$obseInteropSourcePaths = @(
    'ue4ss/mod/OBSE64Interop/CMakeLists.txt',
    'ue4ss/mod/OBSE64Interop/include/OBSE64PluginABI.hpp',
    'ue4ss/mod/OBSE64Interop/include/OBSE64PluginManager.hpp',
    'ue4ss/mod/OBSE64Interop/include/OBSE64PluginScanner.hpp',
    'ue4ss/mod/OBSE64Interop/Scripts/main.lua',
    'ue4ss/mod/OBSE64Interop/src/dllmain.cpp',
    'ue4ss/mod/OBSE64Interop/src/OBSE64PluginManager.cpp',
    'ue4ss/mod/OBSE64Interop/src/OBSE64PluginScanner.cpp'
)

$sourceFiles = @($coreSourcePaths | Sort-Object | ForEach-Object {
    $relative = $_
    $full = Get-CanonicalFile (Join-Path $repositoryRoot $relative)
    if (-not $full.StartsWith($repositoryRoot + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        Fail "source escaped repository root: $relative"
    }
    [ordered]@{
        relativePath = $relative.Replace('\', '/')
        sha256 = Get-Sha256 $full
    }
})
$obseInteropSourceFiles = @($obseInteropSourcePaths | Sort-Object | ForEach-Object {
    $relative = $_
    $full = Get-CanonicalFile (Join-Path $repositoryRoot $relative)
    if (-not $full.StartsWith($repositoryRoot + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        Fail "OBSE64 interoperability source escaped repository root: $relative"
    }
    [ordered]@{
        relativePath = $relative.Replace('\', '/')
        sha256 = Get-Sha256 $full
    }
})

$originalText = Get-Content -LiteralPath $manifestFull -Raw -Encoding utf8
$manifest = $originalText | ConvertFrom-Json
if ([string]$manifest.schema -cne 'UNBSEUE4SSFoundation' -or [int]$manifest.schemaVersion -ne 1) {
    Fail 'unsupported foundation manifest schema'
}
if ($null -eq $manifest.unbseMod.buildGeneratedState.patternsleuthCargoLock) {
    Fail 'pinned patternsleuth Cargo.lock generated-state contract is missing'
}
$generatedState = $manifest.unbseMod.buildGeneratedState

$manifest.foundationId = $foundationId
$manifest.unbseMod = [ordered]@{
    name = 'UNBSE'
    version = $coreVersion
    target = 'UNBSEMod'
    buildConfiguration = 'Game__Shipping__Win64'
    packageDll = 'ue4ss/Mods/UNBSE/dlls/main.dll'
    requiredExports = @(
        'UNBSE_QueryAddonHostV1',
        'UNBSE_QueryMessagingV1',
        'UNBSE_QueryRelocationV1',
        'UNBSE_QueryRuntimeInfoV1',
        'UNBSE_QueryScriptServiceV1',
        'start_mod',
        'uninstall_mod'
    )
    sourceFiles = $sourceFiles
    addonHost = [ordered]@{
        abiVersion = 1
        messagingAbiVersion = 1
        runtimeInfoAbiVersion = 1
        relocationAbiVersion = 1
        compatibilityPolicy = 'report-and-attempt'
        maximumRegisteredAddons = 32
        maximumMessageListeners = 128
    }
    scriptService = [ordered]@{
        abiVersion = 1
        backend = 'ue4ss_lua_v1'
        luaNamespace = 'UNBSE'
        maximumFunctions = 64
        maximumFunctionsPerOwner = 16
        maximumArguments = 8
        defaultDeadlineMilliseconds = 50
        supportsReadOnlyFunctions = $true
        supportsRuntimeWriteFunctions = $true
        genericConsolePassthrough = $false
        mcpOwned = $false
    }
    loader = [ordered]@{
        name = 'UNBSELoader'
        target = 'UNBSELoader'
        buildConfiguration = 'Release'
        sourceDirectory = 'ue4ss/loader'
        packageExecutable = 'UNBSELoader.exe'
        supportedDistribution = 'Steam'
        supportedRuntimeVersion = '1.512.105.0'
        injectionPolicy = 'pinned-ue4ss-after-manager-vfs'
    }
    obse64Interop = [ordered]@{
        name = 'UNBSEOBSE64Interop'
        version = '0.1.0'
        target = 'UNBSEOBSE64Interop'
        buildConfiguration = 'Game__Shipping__Win64'
        sourceDirectory = 'ue4ss/mod/OBSE64Interop'
        packageDll = 'ue4ss/Mods/UNBSEOBSE64Interop/dlls/main.dll'
        enabledFile = 'ue4ss/Mods/UNBSEOBSE64Interop/enabled.txt'
        pluginDirectory = 'OBSE/Plugins'
        compatibilityPolicy = 'report-and-attempt'
        compatibilityClaim = 'unverified-attempt'
        requiredHostCapabilities = @('runtime-info-v1', 'relocation-v1')
        declaredEffects = @('runtime-read', 'runtime-write', 'file-io', 'third-party-native-code-load')
        sourceFiles = $obseInteropSourceFiles
        provenance = [ordered]@{
            abiReferenceRepository = 'https://github.com/ianpatt/obse64'
            abiReferenceCommit = '09bdc6155032c19045feba876886465de0c71743'
            abiReferenceFile = 'obse64/PluginAPI.h'
            implementationPolicy = 'clean-room-no-upstream-implementation'
        }
    }
    packageBoundary = [ordered]@{
        bundledProbes = $false
        bundledMcp = $false
        bundledGameFeatureAddons = $false
        baseObse64Interop = $true
        bundledBlueprintLoader = $true
        modManagerRecognitionStubs = $true
        optionalAddonsDefaultEnabled = $false
    }
    buildGeneratedState = $generatedState
}

$proposedText = ($manifest | ConvertTo-Json -Depth 100) + "`n"
if ($Check) {
    if ($originalText -cne $proposedText) {
        Write-Error 'UNBSE core manifest boundary or source pins are stale.'
        exit 1
    }
    Write-Host 'PASS: UNBSE core manifest boundary and source pins are exact.'
    exit 0
}

if ($originalText -ceq $proposedText) {
    Write-Host 'No UNBSE core manifest changes required.'
    exit 0
}
if ($PSCmdlet.ShouldProcess($manifestFull, 'replace the UNBSE core manifest boundary and source pins')) {
    [IO.File]::WriteAllText($manifestFull, $proposedText, [Text.UTF8Encoding]::new($false))
    Write-Host "Updated UNBSE core manifest boundary to version $coreVersion."
}
