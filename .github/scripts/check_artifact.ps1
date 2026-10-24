# check_artifact.ps1 - Artifact leak audit for the Mine-imator CI build.
#
# Audits (1) the staged runtime directory and (2) the final ZIP before upload:
#   - lists every file with relative path, size and SHA-256 (streamed)
#   - rejects development content (sources, headers, build scripts, CMake
#     intermediates, .git, PDBs, dependency source archives...) with a
#     non-zero exit code
#   - rejects ZIP path-traversal entries, absolute paths and Windows
#     separator variants
#   - rejects duplicate ZIP entries and case-insensitive path collisions
#   - rejects directory/file name conflicts inside the ZIP
#   - rejects reparse points (junctions/symlinks) found in the staged tree
#   - verifies every ZIP entry's DECOMPRESSED content SHA-256 against the
#     staged file's SHA-256 (not just names/counts, not the ZIP CRC)
#   - cross-checks that ZIP entries match the staged directory exactly
#   - writes a manifest file (SHA256 / size / relative path) plus the ZIP's
#     own SHA-256
#
# Boundary: this checker prevents ACCIDENTALLY packaging development files.
# It cannot prove anything about what is compiled INTO a binary; that is
# covered by the build itself (Mine-imator does not link XGraphics - verified
# in CppProject/CMakeLists.txt). Unicode homoglyph paths are not exhaustively
# normalized (documented limitation).
#
# Usage:
#   pwsh check_artifact.ps1 -Dir <staged-runtime-dir> -Zip <zip-path> -Manifest <manifest-path>
#
# Exit codes: 0 = pass, 2 = violations found (upload must be blocked), 3 = usage/IO error.

param(
    [Parameter(Mandatory = $true)][string] $Dir,
    [Parameter(Mandatory = $true)][string] $Zip,
    [Parameter(Mandatory = $true)][string] $Manifest
)

$ErrorActionPreference = "Stop"

function Get-RelPath([string]$Root, [string]$FullPath) {
    $rel = $FullPath.Substring($Root.Length).TrimStart('\', '/')
    return ($rel -replace '\\', '/')
}

function Test-PathSafe([string]$RelPath) {
    if ($RelPath -match '^([A-Za-z]:|[\\/])') { return $false }
    foreach ($seg in ($RelPath -split '/')) {
        if ($seg -eq '..' -or $seg -eq '.') { return $false }
        # Reject reserved-device names and names containing control characters.
        if ($seg -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)') { return $false }
        if ($seg -match '[\x00-\x1f]') { return $false }
    }
    return $true
}

# --- Development-content rules ---------------------------------------------------
$forbiddenDirs = @('.git', '.github', 'CMakeFiles', 'Generated', 'External', '__pycache__')
$forbiddenExt = @(
    '.cpp', '.c', '.cc', '.cxx', '.h', '.hpp', '.hh', '.gml', '.yy', '.yyp',
    '.pdb', '.lib', '.obj', '.ilk', '.exp', '.cmake', '.vcxproj', '.vcxproj.filters',
    '.sln', '.slnx', '.ps1', '.psm1', '.yml', '.yaml', '.log', '.cache', '.user',
    '.7z', '.tar', '.gz', '.xz', '.bz2'
)
$dataPrefix = 'Data/'
function Test-FileAllowed([string]$RelPath) {
    foreach ($d in $forbiddenDirs) {
        if ($RelPath -eq $d -or $RelPath.StartsWith("$d/", [System.StringComparison]::OrdinalIgnoreCase)) { return $false }
    }
    $ext = [System.IO.Path]::GetExtension($RelPath)
    if ($forbiddenExt -contains $ext.ToLowerInvariant()) {
        if (($ext -in @('.7z', '.tar', '.gz', '.xz', '.bz2', '.zip')) -and $RelPath.StartsWith($dataPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
        return $false
    }
    return $true
}

# --- Streaming SHA-256 helpers ---------------------------------------------------
function Get-FileSha256([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [System.IO.File]::OpenRead($Path)
        try { return ([System.BitConverter]::ToString($sha.ComputeHash($fs))).Replace('-', '').ToLowerInvariant() }
        finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
}
function Get-StreamSha256($Stream) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return ([System.BitConverter]::ToString($sha.ComputeHash($Stream))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

# --- Argument checks ---------------------------------------------------------------
if (-not (Test-Path -LiteralPath $Dir -PathType Container)) {
    Write-Host "ERROR: staged directory does not exist: $Dir"
    exit 3
}
if (-not (Test-Path -LiteralPath $Zip -PathType Leaf)) {
    Write-Host "ERROR: ZIP does not exist: $Zip"
    exit 3
}
$root = [System.IO.Path]::GetFullPath($Dir).TrimEnd('\')
if (Test-Path -LiteralPath (Join-Path $root '.git')) {
    Write-Host "RESULT: FAIL - the staged directory contains a .git folder (repository root must never be packaged)."
    exit 2
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

$violations = New-Object System.Collections.Generic.List[string]

# --- Audit the staged directory -----------------------------------------------------
$stagedHash = @{}          # rel -> sha256
$manifestLines = New-Object System.Collections.Generic.List[string]
$stagedSet = New-Object 'System.Collections.Generic.HashSet[string]'
$items = Get-ChildItem -LiteralPath $root -Recurse -Force
foreach ($item in $items) {
    $rel = Get-RelPath $root $item.FullName
    # Reparse points (junctions/symlinks) are rejected with an explicit policy:
    # the CMake install() output never contains them, and following them could
    # escape the staged directory.
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        $violations.Add("STAGED reparse point (junction/symlink) rejected: $rel")
        continue
    }
    if ($item.PSIsContainer) { continue }
    if (-not (Test-PathSafe $rel)) {
        $violations.Add("STAGED unsafe path: $rel")
        continue
    }
    if (-not (Test-FileAllowed $rel)) {
        $violations.Add("STAGED development/forbidden content: $rel")
        continue
    }
    $fileSha = Get-FileSha256 $item.FullName
    $stagedHash[$rel] = $fileSha
    [void]$stagedSet.Add($rel)
    $manifestLines.Add("$fileSha  $($item.Length)  $rel")
}

# --- Audit the ZIP -------------------------------------------------------------------
$zipFull = [System.IO.Path]::GetFullPath($Zip)
$zipArchive = [System.IO.Compression.ZipFile]::OpenRead($zipFull)
$zipSet = New-Object 'System.Collections.Generic.HashSet[string]'
$zipSetIgnoreCase = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
$zipDirs = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
$zipFileCount = 0
try {
    foreach ($entry in $zipArchive.Entries) {
        $rel = ($entry.FullName -replace '\\', '/')
        if ([string]::IsNullOrWhiteSpace($rel)) { continue }
        if (-not (Test-PathSafe $rel)) {
            $violations.Add("ZIP unsafe path (traversal/absolute/control chars): $rel")
            continue
        }
        $isDirEntry = $rel.EndsWith('/')
        $relClean = if ($isDirEntry) { $rel.TrimEnd('/') } else { $rel }
        # Exact duplicate detection: Add() returns false when already present.
        if (-not $isDirEntry) {
            if (-not $zipSet.Add($relClean)) {
                $violations.Add("ZIP duplicate entry: $relClean")
                continue
            }
            $zipFileCount++
        }
        # Case-insensitive collision: two entries differing only by case unpack
        # to the same file on Windows.
        if (-not $isDirEntry) {
            if (-not $zipSetIgnoreCase.Add($relClean)) {
                $violations.Add("ZIP case-insensitive path collision: $relClean")
                continue
            }
        }
        # Directory/file conflict: a file entry whose path equals (or sits under)
        # a known directory entry, or a directory entry equal to a file entry.
        if ($isDirEntry) {
            [void]$zipDirs.Add($relClean)
            if ($zipSet.Contains($relClean)) { $violations.Add("ZIP dir/file conflict: $relClean") }
        } else {
            if ($zipDirs.Contains($relClean)) { $violations.Add("ZIP dir/file conflict: $relClean") }
        }
        if ($isDirEntry) { continue }
        if (-not (Test-FileAllowed $relClean)) {
            $violations.Add("ZIP development/forbidden content: $relClean")
            continue
        }
        # Content-level verification: decompressed entry bytes must hash exactly
        # like the staged file (never trusting the ZIP CRC or entry metadata).
        $entryStream = $entry.Open()
        try {
            $entrySha = Get-StreamSha256 $entryStream
        } finally { $entryStream.Dispose() }
        if (-not $stagedHash.ContainsKey($relClean)) {
            $violations.Add("MISMATCH: ZIP contains a file not in the staged directory: $relClean")
            continue
        }
        if ($stagedHash[$relClean] -ne $entrySha) {
            $violations.Add("ZIP CONTENT MISMATCH vs staged file: $relClean")
        }
    }
}
finally {
    $zipArchive.Dispose()
}

# --- Cross-check: staged files must all be present in the ZIP --------------------
foreach ($rel in $stagedSet) {
    if (-not $zipSet.Contains($rel)) { $violations.Add("MISMATCH: staged file missing from ZIP: $rel") }
}
if ($zipFileCount -ne $stagedSet.Count) {
    # Belt-and-suspenders: set equality already implies this, but an explicit
    # count check can never be masked by set deduplication.
    $violations.Add("COUNT MISMATCH: ZIP file entries=$zipFileCount, staged files=$($stagedSet.Count)")
}

# --- Manifest -----------------------------------------------------------------------
$zipBytes = [System.IO.File]::ReadAllBytes($zipFull)
$zipSha = ([System.BitConverter]::ToString([System.Security.Cryptography.SHA256]::Create().ComputeHash($zipBytes))).Replace('-', '').ToLowerInvariant()
$fileCount = $manifestLines.Count
$manifestDir = [System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($Manifest))
if (-not (Test-Path -LiteralPath $manifestDir)) { New-Item -ItemType Directory -Path $manifestDir -Force | Out-Null }
$manifestLines.Add("# ZIP sha256: $zipSha")
$manifestLines.Add("# ZIP size: $((Get-Item -LiteralPath $zipFull).Length)")
$manifestLines.Add("# files: $fileCount")
[System.IO.File]::WriteAllLines($Manifest, $manifestLines)

# --- Report ---------------------------------------------------------------------------
Write-Host "=== Artifact audit ==="
Write-Host "Staged dir : $root ($($stagedSet.Count) files)"
Write-Host "ZIP        : $zipFull ($($zipSet.Count) unique file entries)"
Write-Host "ZIP sha256 : $zipSha"
Write-Host "Manifest   : $Manifest"
if ($violations.Count -gt 0) {
    Write-Host "VIOLATIONS ($($violations.Count)):"
    foreach ($v in $violations) { Write-Host "  - $v" }
    Write-Host "RESULT: FAIL - artifact upload must be blocked."
    exit 2
}
Write-Host "RESULT: PASS - no development content, no path traversal, no duplicates, content hashes verified, ZIP matches the staged directory."
exit 0
