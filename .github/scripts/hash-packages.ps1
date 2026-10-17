# Builds the hash packages into -Out with the GrayWolf.exe given: the hand-maintained base (asset
# package_index.zip of the prerelease tagged `hash-packages`: BO3 script hashes, BO4 recovered
# names, ...) with echo000/cod-name-db's current CSVs merged over it by update-name-db. Used by the
# release (build.yml) and by the rebuild of a release's packages when their sources move
# (hash-packages.yml).
#
# -SourcesOnly: only prints what the packages would be built from ("cod-name-db@<commit>
# base@<asset update time>"), the manifest's packages.sources, which tells whether a rebuild would
# change anything.
param(
    [Parameter(Mandatory)] [string] $Repo,
    [string] $Exe,
    [string] $Out = 'pkg',
    [switch] $SourcesOnly
)
$ErrorActionPreference = 'Stop'

function Sources {
    $nameDb = gh api repos/echo000/cod-name-db/commits/main --jq .sha
    if ($LASTEXITCODE -ne 0 -or -not $nameDb) { throw 'cod-name-db: no commit for main' }
    $base = gh release view hash-packages --repo $Repo --json assets --jq '.assets[] | select(.name == "package_index.zip") | .updatedAt' 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $base) { $base = 'none'; $global:LASTEXITCODE = 0 }
    "cod-name-db@$nameDb base@$base"
}

if ($SourcesOnly) { Sources; exit 0 }

$sources = Sources
New-Item -ItemType Directory -Force $Out, base | Out-Null
gh release download hash-packages --repo $Repo --pattern package_index.zip --dir base --clobber 2>$null
if ($LASTEXITCODE -eq 0) { Expand-Archive base/package_index.zip -DestinationPath $Out -Force }
else { Write-Warning "no hash-packages base release: cod-name-db's CSVs only"; $global:LASTEXITCODE = 0 }
$p = Start-Process $Exe -ArgumentList 'update-name-db', '--name-db-dir', (Resolve-Path $Out).Path, '--report', "$PWD/name-db.txt" -Wait -PassThru
Get-Content name-db.txt | Write-Host
if ($p.ExitCode -gt 1) { throw "update-name-db failed ($($p.ExitCode))" }
# the updater stamps its own hash-packages.json, a base's copy would be stale
Remove-Item "$Out/hash-packages.json" -ErrorAction SilentlyContinue
$sources
