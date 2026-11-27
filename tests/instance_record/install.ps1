<#
.SYNOPSIS
Installs the instance record tests (phase 2, WEAPON_ENGINE_PLAN.md 4 and 5) into an Anomaly install.

.DESCRIPTION
Copies tests\instance_record\gamedata into <Target>\gamedata. With -Exe, also copies the
built engine into <Target>\bin, keeping the original as AnomalyDX11_vanilla.exe the first time.
Use -WhatIf to see what would be copied.

.EXAMPLE
.\install.ps1 -Target D:\Anomaly3 -Exe
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
	[string]$Target = 'D:\Anomaly3',
	[switch]$Exe
)

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$gamedata = Join-Path $Target 'gamedata'

if (-not (Test-Path (Join-Path $Target 'fsgame.ltx'))) {
	throw "No fsgame.ltx in $Target. Is it an Anomaly install?"
}

$from = (Resolve-Path (Join-Path $PSScriptRoot 'gamedata')).Path
Get-ChildItem -LiteralPath $from -Recurse -File | ForEach-Object {
	$rel = $_.FullName.Substring($from.Length).TrimStart('\')
	$dest = Join-Path $gamedata $rel
	if ($PSCmdlet.ShouldProcess($dest, "copy from $($_.FullName)")) {
		New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
		Copy-Item -LiteralPath $_.FullName -Destination $dest -Force
	}
}

if ($Exe) {
	$built = Join-Path $repo '_build\_game\bin_dbg\AnomalyDX11.exe'
	$dest = Join-Path $Target 'bin\AnomalyDX11.exe'
	$vanilla = Join-Path $Target 'bin\AnomalyDX11_vanilla.exe'
	if (-not (Test-Path $vanilla) -and (Test-Path $dest) -and $PSCmdlet.ShouldProcess($vanilla, 'keep the original exe')) {
		Copy-Item -LiteralPath $dest -Destination $vanilla
	}
	if ($PSCmdlet.ShouldProcess($dest, "copy from $built")) {
		Copy-Item -LiteralPath $built -Destination $dest -Force
	}
}

Write-Host "Done. In game: run_string mw_ir.api(), then see the header of mw_ir.script."
