[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$descriptor = Get-Content -LiteralPath (Join-Path $repoRoot 'PaperZDMCP/PaperZDMCP.uplugin') -Raw | ConvertFrom-Json
$version = $descriptor.VersionName
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'VersionName must be a semantic numeric version.' }
$dist = Join-Path $repoRoot 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$archive = Join-Path $dist "PaperZDMCP-$version-source.zip"
if (Test-Path -LiteralPath $archive) { throw "Release already exists: $archive. Choose a new version or move the old release explicitly." }
$stage = Join-Path $repoRoot ('.build/package-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'PaperZDMCP') | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'PaperZDMCP/Source') -Destination (Join-Path $stage 'PaperZDMCP/Source') -Recurse
Copy-Item -LiteralPath (Join-Path $repoRoot 'PaperZDMCP/PaperZDMCP.uplugin') -Destination (Join-Path $stage 'PaperZDMCP')
foreach ($entry in @('README.md', 'LICENSE', 'CHANGELOG.md', 'docs')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $entry) -Destination $stage -Recurse
}
foreach ($entry in @('scripts', 'tests')) {
    $inputRoot = Join-Path $repoRoot $entry
    foreach ($sourceFile in Get-ChildItem -LiteralPath $inputRoot -Recurse -File) {
        $relative = $sourceFile.FullName.Substring($inputRoot.Length + 1)
        if ($relative -match '(^|[\\/])__pycache__([\\/]|$)' -or $sourceFile.Extension -in @('.pyc', '.pyo')) { continue }
        $destinationFile = Join-Path (Join-Path $stage $entry) $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destinationFile) | Out-Null
        Copy-Item -LiteralPath $sourceFile.FullName -Destination $destinationFile
    }
}
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath "$archive.sha256" -Value "$hash  $([IO.Path]::GetFileName($archive))" -Encoding ascii
Write-Output $archive
Write-Output "$archive.sha256"
