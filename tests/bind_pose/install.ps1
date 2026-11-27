<#
.SYNOPSIS
Installs the bind-pose test (engine change 1a) into a plain Anomaly install.

.DESCRIPTION
Copies into <Target>\gamedata:
  1. this repository's engine gamedata (scripts and configs that match the engine build)
  2. the test gun and its test script (tests\bind_pose\gamedata)
  3. the test model, wpn_ak74_n_one.ogf, from Part_bench
  4. the model's texture and motion file from the local BaS mod in GAMMA
     (other authors' assets: for local testing only, never ship them)
Optionally copies the built engine executable into <Target>\bin, keeping the
original as AnomalyDX11_vanilla.exe the first time.

No Mod Organizer is involved: the files go straight into the install.
Use -WhatIf to see what would be copied.

.EXAMPLE
.\install.ps1 -Target D:\Anomaly3 -Exe
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
	[string]$Target = 'D:\Anomaly3',
	[string]$GammaMods = 'D:\GAMMA\mods',
	[string]$PartBench = (Join-Path $PSScriptRoot '..\..\..\part_bench'),
	[switch]$Exe
)

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$gamedata = Join-Path $Target 'gamedata'

if (-not (Test-Path (Join-Path $Target 'fsgame.ltx'))) {
	throw "No fsgame.ltx in $Target. Is it an Anomaly install?"
}

function Copy-Tree([string]$from, [string]$to) {
	Get-ChildItem -LiteralPath $from -Recurse -File | ForEach-Object {
		$rel = $_.FullName.Substring($from.Length).TrimStart('\')
		$dest = Join-Path $to $rel
		if ($PSCmdlet.ShouldProcess($dest, "copy from $($_.FullName)")) {
			New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
			Copy-Item -LiteralPath $_.FullName -Destination $dest -Force
		}
	}
}

function Copy-One([string]$from, [string]$rel) {
	$dest = Join-Path $gamedata $rel
	if ($PSCmdlet.ShouldProcess($dest, "copy from $from")) {
		New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
		Copy-Item -LiteralPath $from -Destination $dest -Force
	}
}

# 1. Engine gamedata matching this build
$engineData = (Resolve-Path (Join-Path $repo 'gamedata')).Path
Write-Host "Engine gamedata: $engineData"
Copy-Tree $engineData $gamedata

# 2. Test gun and script
$kitData = (Resolve-Path (Join-Path $PSScriptRoot 'gamedata')).Path
Write-Host "Test kit: $kitData"
Copy-Tree $kitData $gamedata

# 3. Test model from Part_bench
$model = Join-Path $PartBench 'gamedata\meshes\mw\wpn_ak74_n_one.ogf'
if (-not (Test-Path $model)) { throw "Test model not found: $model" }
Copy-One (Resolve-Path $model).Path 'meshes\mw\wpn_ak74_n_one.ogf'

# 4. Assets the model references, from whichever GAMMA mod has them
# Every texture set the model's parts use (the "Can't find texture" list in the log)
$parts = @('body', 'bolt', 'mag_30rnd', 'handguard', 'reciever', 'kolodka', 'muzzle', 'sight_rear',
	'mount_dovetail', 'pistolgrip_bakelite', 'gl_gp34', 'silencer_atg', 'ammo_545x39')
$assets = @('meshes\dynamics\weapons\wpn_ak74m_n\wpn_ak74m_hud_animation.omf')
$optional = @()
foreach ($p in $parts) {
	$t = "textures\wpn\wpn_eft_ak74_$p"
	$assets += "${t}_diff.dds", "${t}_bump.dds", "${t}_bump#.dds"
	$optional += "${t}_diff.thm", "${t}_bump.thm"   # some parts ship without a bump .thm
}
$mods = Get-ChildItem -LiteralPath $GammaMods -Directory | Sort-Object Name
function Find-InMods([string]$rel) {
	foreach ($mod in $mods) {
		$candidate = Join-Path $mod.FullName (Join-Path 'gamedata' $rel)
		if (Test-Path -LiteralPath $candidate) { return @($candidate, $mod.Name) }
	}
}
foreach ($rel in $assets + $optional) {
	$hit = Find-InMods $rel
	if (-not $hit) {
		if ($optional -contains $rel) { Write-Host "  $rel (none, skipped)"; continue }
		throw "Not found in any mod under ${GammaMods}: $rel"
	}
	Write-Host "  $rel <- $($hit[1])"
	Copy-One $hit[0] $rel
}

# 5. Optional: the engine build
if ($Exe) {
	$built = Join-Path $repo '_build\_game\bin_dbg\AnomalyDX11.exe'
	if (-not (Test-Path $built)) { throw "No built executable at $built" }
	$dest = Join-Path $Target 'bin\AnomalyDX11.exe'
	$backup = Join-Path $Target 'bin\AnomalyDX11_vanilla.exe'
	if ((Test-Path $dest) -and -not (Test-Path $backup)) {
		if ($PSCmdlet.ShouldProcess($backup, "back up $dest")) { Copy-Item $dest $backup }
	}
	if ($PSCmdlet.ShouldProcess($dest, "copy engine build")) { Copy-Item $built $dest -Force }
}

Write-Host "Done. Start $Target\bin\AnomalyDX11.exe with -dbg, then: run_string mw_bp.give()"
