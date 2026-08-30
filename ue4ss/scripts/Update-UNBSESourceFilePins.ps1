[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$ManifestPath = (Join-Path $PSScriptRoot '..\foundation-manifest.json'),
    [switch]$Check
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) { throw "UNBSE source-pin updater: $Message" }

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

function Assert-NoReparsePath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { Fail "target is missing or is not a file: $full" }
    $probe = $full
    while ($true) {
        $item = Get-Item -LiteralPath $probe -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { Fail "reparse target is not permitted: $probe" }
        $parent = Split-Path -Path $probe -Parent
        if ($parent -eq $probe -or [string]::IsNullOrWhiteSpace($parent)) { break }
        $probe = $parent
    }
    return $full
}

function Get-Collection([object]$Root, [string[]]$Segments, [string]$Name) {
    $current = $Root
    foreach ($segment in $Segments) {
        $property = $current.PSObject.Properties[$segment]
        if ($null -eq $property) { Fail "required collection is missing: $Name" }
        $current = $property.Value
    }
    if ($null -eq $current -or $current -isnot [System.Collections.IEnumerable] -or $current -is [string]) { Fail "required collection is not an array: $Name" }
    return @($current)
}

try {
    $manifestFull = Assert-NoReparsePath $ManifestPath
    $projectRoot = Split-Path -Path (Split-Path -Path $manifestFull -Parent) -Parent
    $projectRoot = [IO.Path]::GetFullPath($projectRoot).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    if (-not (Test-Path -LiteralPath $projectRoot -PathType Container)) { Fail "derived project root is missing: $projectRoot" }
    $manifestText = Get-Content -LiteralPath $manifestFull -Raw -Encoding utf8
    $manifest = $manifestText | ConvertFrom-Json
    $collectionPaths = @(
        @{ Name = 'patchSet.patches'; Segments = @('patchSet', 'patches') },
        @{ Name = 'unbseMod.sourceFiles'; Segments = @('unbseMod', 'sourceFiles') },
        @{ Name = 'unbseMod.obse64Interop.sourceFiles'; Segments = @('unbseMod', 'obse64Interop', 'sourceFiles') }
    )
    $drift = [System.Collections.Generic.List[string]]::new()
    $replacements = [System.Collections.Generic.List[object]]::new()
    foreach ($spec in $collectionPaths) {
        $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in (Get-Collection $manifest $spec.Segments $spec.Name)) {
            if ($null -eq $entry -or $null -eq $entry.PSObject.Properties['relativePath'] -or $null -eq $entry.PSObject.Properties['sha256']) { Fail "invalid source-file entry in $($spec.Name)" }
            $relative = [string]$entry.relativePath
            if ([string]::IsNullOrWhiteSpace($relative) -or [IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[\\/])\.\.([\\/]|$)') { Fail "out-of-repository target in $($spec.Name): $relative" }
            $target = [IO.Path]::GetFullPath((Join-Path $projectRoot $relative))
            if (-not $target.StartsWith($projectRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { Fail "out-of-repository target in $($spec.Name): $relative" }
            if (-not $seen.Add($relative)) { Fail "duplicate target in $($spec.Name): $relative" }
            $target = Assert-NoReparsePath $target
            $actual = Get-Sha256 $target
            if ([string]$entry.sha256 -ne $actual) {
                $drift.Add("$($spec.Name): $relative")
                if (-not $Check) { $replacements.Add([pscustomobject]@{ RelativePath = $relative; OldHash = [string]$entry.sha256; NewHash = $actual }) }
            }
        }
    }
    if ($Check) {
        if ($drift.Count) { $drift | ForEach-Object { Write-Error "stale SHA-256 pin: $_" }; exit 1 }
        Write-Host 'PASS: all UNBSE source-file SHA-256 pins match.'
        exit 0
    }
    if ($drift.Count) {
        $updated = $manifestText
        foreach ($group in ($replacements | Group-Object { "$($_.RelativePath)`0$($_.OldHash)`0$($_.NewHash)" })) {
            $entry = $group.Group[0]
            $pattern = '("relativePath"\s*:\s*"' + [regex]::Escape($entry.RelativePath) + '"\s*,\s*"sha256"\s*:\s*")' + [regex]::Escape($entry.OldHash) + '(")'
            $matches = [regex]::Matches($updated, $pattern)
            if ($matches.Count -ne $group.Count) {
                $pattern = '("sha256"\s*:\s*")' + [regex]::Escape($entry.OldHash) + '("\s*,\s*"relativePath"\s*:\s*"' + [regex]::Escape($entry.RelativePath) + '")'
                $matches = [regex]::Matches($updated, $pattern)
            }
            if ($matches.Count -ne $group.Count) { Fail "source-file entry anchor is ambiguous or missing for $($entry.RelativePath)" }
            $updated = [regex]::Replace($updated, $pattern, { param($match) "$($match.Groups[1].Value)$($entry.NewHash)$($match.Groups[2].Value)" })
        }
        [IO.File]::WriteAllText($manifestFull, $updated, [Text.UTF8Encoding]::new($false))
        Write-Host "Updated $($drift.Count) UNBSE source-file SHA-256 pin(s)."
    } else { Write-Host 'No UNBSE source-file SHA-256 pin changes required.' }
    exit 0
} catch {
    Write-Error $_.Exception.Message
    exit 1
}
