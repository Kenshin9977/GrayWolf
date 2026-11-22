# A side-by-side variant of zm_bo1_mp_nuked: usermaps\<Name> from the baseline's text files (names replaced),
# map_source\zm\<Name>.map from the baseline's level .map (optionally another prefab, without umbra_volume),
# then the four tools GrayWolf runs (gdtdb, cod2map with extra args, light, link), logs in usermaps\<Name>.
param([string]$Name, [string]$Prefab = 'graywolf/bo1_mp_nuked.map', [string[]]$Cod2mapExtra = @(), [switch]$NoUmbraVolume, [switch]$SkipGdtdb, [string]$Base = 'zm_bo1_mp_nuked', [string]$AddText = '')
$ErrorActionPreference = 'Stop'
$Root = 'C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops III'
$Src = Join-Path $Root "usermaps\$Base"
$Dst = Join-Path $Root "usermaps\$Name"
foreach ($d in 'scripts\zm', 'zone_source', 'sound\zoneconfig', 'zone') { New-Item -ItemType Directory -Force (Join-Path $Dst $d) | Out-Null }
function CopyText($from, $to) { [IO.File]::WriteAllText($to, [IO.File]::ReadAllText($from).Replace($Base, $Name)) }
foreach ($f in Get-ChildItem (Join-Path $Src 'scripts\zm') -File) {
    CopyText $f.FullName (Join-Path $Dst ('scripts\zm\' + $f.Name.Replace($Base, $Name)))
}
CopyText (Join-Path $Src "zone_source\$Base.zone") (Join-Path $Dst "zone_source\$Name.zone")
CopyText (Join-Path $Src "sound\zoneconfig\$Base.szc") (Join-Path $Dst "sound\zoneconfig\$Name.szc")
foreach ($f in 'loadingimage.png', 'previewimage.png') { Copy-Item (Join-Path $Src "zone\$f") (Join-Path $Dst "zone\$f") -Force }
$id = 1000000000 + [Math]::Abs($Name.GetHashCode()) % 899999999
[IO.File]::WriteAllText((Join-Path $Dst 'zone\workshop.json'), "{`n`t`"PublisherID`": `"$id`",`n`t`"Title`": `"$Name (GrayWolf test)`",`n`t`"Description`": `"culling test variant`",`n`t`"FolderName`": `"$Name`",`n`t`"Type`": `"map`"`n}`n")
# the level .map
$lvl = [IO.File]::ReadAllText((Join-Path $Root "map_source\zm\$Base.map"))
$lvl = $lvl.Replace('"model" "graywolf/bo1_mp_nuked.map"', "`"model`" `"$Prefab`"")
if ($NoUmbraVolume) {
    $i = $lvl.IndexOf('"classname" "umbra_volume"')
    $s = $lvl.LastIndexOf("`n{", $i)
    # the entity: its opening brace to the brace closing it (after its brush's)
    $depth = 0; $k = $s + 1
    for (; $k -lt $lvl.Length; $k++) { if ($lvl[$k] -eq '{') { $depth++ } elseif ($lvl[$k] -eq '}') { $depth--; if ($depth -eq 0) { break } } }
    $lvl = $lvl.Substring(0, $s + 1) + $lvl.Substring($k + 1)
    if ($lvl.Contains('umbra_volume')) { throw 'umbra_volume still there' }
}
$lvl = $lvl + $AddText
[IO.File]::WriteAllText((Join-Path $Root "map_source\zm\$Name.map"), $lvl)
# the tools, as GrayWolf's ModToolsUsermap::Build runs them
$env:TA_GAME_PATH = "$Root\"; $env:TA_TOOLS_PATH = "$Root\"; $env:TA_LOCAL_ASSET_CACHE = "$Root\share\assetconvert\"
$Level = Join-Path $Root "map_source\zm\$Name.map"
$steps = @()
if (-not $SkipGdtdb) { $steps += , @('gdtdb', "$Root\gdtdb\gdtdb.exe", @('/update')) }
$steps += , @('compile', "$Root\bin\cod2map64.exe", (@('-platform', 'pc', '-navmesh', '-navvolume') + $Cod2mapExtra + @('-loadFrom', "`"$Level`"", "`"$Root\share\raw\maps\zm\$Name.d3dbsp`"")))
$steps += , @('light', "$Root\bin\radiant_modtools.exe", @('-ledSilent', '+medium', '+localprobes', '+forceclean', '+recompute', "`"$Level`""))
$steps += , @('link', "$Root\bin\linker_modtools.exe", @('-language', 'english', '-modsource', $Name))
foreach ($st in $steps) {
    $log = Join-Path $Dst ("graywolf_" + $st[0] + ".log")
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $st[1] -ArgumentList $st[2] -WorkingDirectory (Split-Path $st[1]) -NoNewWindow -Wait -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $err = (Select-String -Path $log -Pattern 'ERROR' -ErrorAction SilentlyContinue | Measure-Object).Count
    "$Name $($st[0]): exit $($p.ExitCode) in $([int]$sw.Elapsed.TotalSeconds)s, $err ERROR lines"
}
$ff = Join-Path $Dst "zone\$Name.ff"
"$Name ff: " + $(if (Test-Path $ff) { (Get-Item $ff).LastWriteTime } else { 'missing' })
