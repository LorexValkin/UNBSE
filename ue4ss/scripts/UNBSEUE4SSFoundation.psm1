Set-StrictMode -Version Latest

function Get-UNBSECanonicalPath {
    param([Parameter(Mandatory)][string]$Path, [switch]$MustExist)
    $full = [IO.Path]::GetFullPath($Path)
    if ($MustExist -and -not (Test-Path -LiteralPath $full)) { throw "Path does not exist: $full" }
    return $full.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
}

function Assert-UNBSENoReparsePath {
    param([Parameter(Mandatory)][string]$Path, [switch]$AllowMissingLeaf)
    $candidate = Get-UNBSECanonicalPath $Path
    $probe = $candidate
    if ($AllowMissingLeaf -and -not (Test-Path -LiteralPath $probe)) { $probe = Split-Path -Path $probe -Parent }
    while ($true) {
        if (Test-Path -LiteralPath $probe) {
            $item = Get-Item -LiteralPath $probe -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Reparse point is not permitted: $probe" }
        }
        $parent = Split-Path -Path $probe -Parent
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $probe) { break }
        $probe = $parent
    }
    return $candidate
}

function Get-UNBSEHash {
    param([string]$Path)
    $stream = [IO.File]::OpenRead($Path)
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

function Get-UNBSEHashBytes {
    param([byte[]]$Bytes)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha256.ComputeHash($Bytes))).Replace('-', '')
    }
    finally {
        $sha256.Dispose()
    }
}
function Test-UNBSEExactFile {
    param([string]$Path, [string]$Sha256, [long]$Bytes, [string]$Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "$Label is missing: $Path" }
    try { Assert-UNBSENoReparsePath $Path | Out-Null } catch { return $_.Exception.Message }
    if ((Get-Item -LiteralPath $Path).Length -ne $Bytes) { return "$Label has an unexpected length: $Path" }
    if ((Get-UNBSEHash $Path) -ne $Sha256.ToUpperInvariant()) { return "$Label has an unexpected SHA-256: $Path" }
    return $null
}

function ConvertFrom-UNBSEIni {
    param([Parameter(Mandatory)][string]$Path, [hashtable]$RequiredSettings = @{})
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return [pscustomobject]@{ Lines=@(); Values=@{}; Errors=@("Settings file is missing: $Path"); Sections=@{} } }
    Assert-UNBSENoReparsePath $Path | Out-Null
    $lines = [IO.File]::ReadAllLines($Path)
    $values = @{}; $sections = @{}; $errors = [Collections.Generic.List[string]]::new(); $section = $null
    foreach ($required in $RequiredSettings.Keys) { $parts=$required.Split('.',2); if (-not $sections.ContainsKey($parts[0])) {$sections[$parts[0]]=@()} }
    for ($i=0; $i -lt $lines.Count; $i++) {
        $line=$lines[$i]; $trim=$line.Trim()
        if ($trim -match '^\[([^\]]+)\]\s*$') {
            $section=$Matches[1]
            if ($sections.ContainsKey($section)) { $sections[$section] += $i; if ($sections[$section].Count -gt 1) {$errors.Add("Duplicate required section [$section]")} }
            continue
        }
        if ($null -ne $section -and $trim -notmatch '^[;#]' -and $line -match '^\s*([^=;#\s][^=]*)\s*=\s*(.*?)\s*$') {
            $key=$Matches[1].Trim(); $value=$Matches[2].Trim(); $full="$section.$key"
            if ($RequiredSettings.ContainsKey($full)) {
                if ($values.ContainsKey($full)) { $errors.Add("Duplicate required key $full") } else { $values[$full]=[pscustomobject]@{ Value=$value; Line=$i; Section=$section; Key=$key } }
            }
        }
    }
    foreach ($required in $RequiredSettings.Keys) {
        if (-not $values.ContainsKey($required)) { $errors.Add("Missing required setting $required") }
        elseif ($values[$required].Value -ne [string]$RequiredSettings[$required]) { $errors.Add("Required setting $required is '$($values[$required].Value)', expected '$($RequiredSettings[$required])'") }
    }
    [pscustomobject]@{ Lines=$lines; Values=$values; Errors=@($errors); Sections=$sections }
}

function Get-UNBSEFoundationAudit {
    param([Parameter(Mandatory)][string]$GameRoot, [string]$SourceRoot = (Join-Path $PSScriptRoot '..\..\..\UE4SS'), [string]$ManifestPath = (Join-Path $PSScriptRoot '..\foundation-manifest.json'))
    $errors=[Collections.Generic.List[string]]::new()
    $warnings=[Collections.Generic.List[string]]::new()
    try { $game=Assert-UNBSENoReparsePath (Get-UNBSECanonicalPath $GameRoot -MustExist) } catch { $errors.Add($_.Exception.Message); return [pscustomobject]@{ Success=$false; Compatibility='blocked'; Errors=@($errors); Warnings=@($warnings); GameRoot=$GameRoot } }
    try { $source=Assert-UNBSENoReparsePath (Get-UNBSECanonicalPath $SourceRoot -MustExist); $manifestFile=Assert-UNBSENoReparsePath (Get-UNBSECanonicalPath $ManifestPath -MustExist); $manifest=Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json } catch { $errors.Add($_.Exception.Message); return [pscustomobject]@{ Success=$false; Compatibility='blocked'; Errors=@($errors); Warnings=@($warnings); GameRoot=$game } }
    foreach ($name in 'OblivionRemastered-Win64-Shipping','UNBSELoader') { if (Get-Process -Name $name -ErrorAction SilentlyContinue) {$errors.Add("Refusing while process is running: $name")} }
    $required=@{}; $manifest.requiredSettings.psobject.Properties | ForEach-Object { $required[$_.Name]=[string]$_.Value }
    $exe=Join-Path $game $manifest.supportedGame.executable
    # The game manifest deliberately has no byte pin; derive its actual size only after existence, while SHA remains exact.
    if (-not (Test-Path -LiteralPath $exe)) {$errors.Add("Game executable is missing: $exe")} else {
        try { Assert-UNBSENoReparsePath $exe | Out-Null } catch { $errors.Add($_.Exception.Message) }
        if ((Get-UNBSEHash $exe) -ne $manifest.supportedGame.sha256.ToUpperInvariant()) {
            if ([string]$manifest.supportedGame.compatibilityPolicy -ceq 'report-and-attempt') {
                $warnings.Add("UNVERIFIED ATTEMPT: game executable SHA-256 is not the verified build; UNBSE will permit launch and runtime add-ons may fail or crash: $exe")
            } else {
                $errors.Add("Game executable has an unexpected SHA-256 and the manifest does not permit report-and-attempt: $exe")
            }
        }
    }
    foreach ($artifact in $manifest.requiredRuntimeArtifacts) {
        $sourceRelative = [string]$artifact.relativePath
        $targetRelative = if ($artifact.PSObject.Properties.Name -contains 'packageRelativePath') {
            [string]$artifact.packageRelativePath
        } else { $sourceRelative }
        $targetSha256 = if ($artifact.PSObject.Properties.Name -contains 'packageSha256') {
            [string]$artifact.packageSha256
        } else { [string]$artifact.sha256 }
        $targetBytes = if ($artifact.PSObject.Properties.Name -contains 'packageBytes') {
            [long]$artifact.packageBytes
        } else { [long]$artifact.bytes }
        $sourceFile=Join-Path $source $sourceRelative; $targetFile=Join-Path $game $targetRelative
        $err=Test-UNBSEExactFile $sourceFile $artifact.sha256 ([long]$artifact.bytes) "Source artifact $($artifact.relativePath)"; if ($err) {$errors.Add($err)}
        $err=Test-UNBSEExactFile $targetFile $targetSha256 $targetBytes "Target artifact $targetRelative"; if ($err) {$errors.Add($err)}
    }
    $ini=ConvertFrom-UNBSEIni (Join-Path $game 'ue4ss/UE4SS-settings.ini') $required
    foreach($x in $ini.Errors){$errors.Add($x)}
    $compatibility = if ($errors.Count -ne 0) { 'blocked' } elseif ($warnings.Count -ne 0) { 'unverified-attempt' } else { 'verified' }
    [pscustomobject]@{ Success=($errors.Count -eq 0); Compatibility=$compatibility; Errors=@($errors); Warnings=@($warnings); GameRoot=$game; SourceRoot=$source; Manifest=$manifest }
}

function Set-UNBSERequiredIni {
    param([string]$Path,[hashtable]$RequiredSettings)
    $parsed=ConvertFrom-UNBSEIni $Path $RequiredSettings
    foreach($error in $parsed.Errors) { if ($error -match '^Duplicate required') { throw $error } }
    $lines=[Collections.Generic.List[string]]::new(); $lines.AddRange([string[]]$parsed.Lines)
    $bySection=@{}; foreach($key in $RequiredSettings.Keys){$section,$name=$key.Split('.',2); if(-not $bySection.ContainsKey($section)){$bySection[$section]=@{}}; $bySection[$section][$name]=[string]$RequiredSettings[$key]}
    # First replace every existing required key while the parser's original line indexes are still
    # valid. Inserting a missing key before this pass shifts every later section and can rewrite the
    # wrong line, which previously duplicated ObjectDumper/CXXHeaderGenerator and omitted Hooks.
    foreach($entry in @($parsed.Values.Values | Sort-Object Line)) {
        $lines[$entry.Line]="$($entry.Key) = $($bySection[$entry.Section][$entry.Key])"
        [void]$bySection[$entry.Section].Remove($entry.Key)
    }
    # Add only the still-missing keys. Re-find each existing section in the current list so insertions
    # made for an earlier section cannot invalidate a stored line index.
    foreach($section in @($bySection.Keys | Sort-Object)){
        if($bySection[$section].Count -eq 0) { continue }
        if(-not $parsed.Sections.ContainsKey($section) -or $parsed.Sections[$section].Count -eq 0) {
            if($lines.Count -gt 0 -and $lines[$lines.Count-1] -ne '') { $lines.Add('') }
            $lines.Add("[$section]")
            foreach($name in @($bySection[$section].Keys | Sort-Object)){ $lines.Add("$name = $($bySection[$section][$name])") }
            continue
        }
        $header=-1
        for($i=0;$i -lt $lines.Count;$i++){if($lines[$i].Trim() -ceq "[$section]"){$header=$i;break}}
        if($header -lt 0){throw "Required section [$section] disappeared during INI update"}
        $insert=$header+1
        while($insert -lt $lines.Count -and $lines[$insert] -notmatch '^\s*\['){$insert++}
        foreach($name in @($bySection[$section].Keys | Sort-Object)){ $lines.Insert($insert,"$name = $($bySection[$section][$name])"); $insert++ }
    }
    [IO.File]::WriteAllLines($Path,$lines,[Text.UTF8Encoding]::new($false))
}

Export-ModuleMember -Function Get-UNBSEFoundationAudit,Set-UNBSERequiredIni,Assert-UNBSENoReparsePath,Get-UNBSEHash,Get-UNBSEHashBytes
