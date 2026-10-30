param(
    [Parameter(Mandatory = $true)]
    [string]$SysmoduleNsp,
    [string]$OutputZip = "switch-richpresence-sysmodule.zip",
    [switch]$EnableAutostart,
    [switch]$UpdateOnly
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$sourceNsp = (Resolve-Path $SysmoduleNsp).Path
$stage = Join-Path $root $(if ($UpdateOnly) { "dist\switch-richpresence-update" } else { "dist\switch-richpresence-sysmodule" })
$id = "4200000000075250"
$contents = Join-Path $stage "atmosphere\contents\$id"
$configDir = Join-Path $stage "config"
$zipPath = Join-Path $root $OutputZip
$configPath = Join-Path $PSScriptRoot "config\switch-richpresence.ini"

if (-not (Test-Path -LiteralPath $sourceNsp -PathType Leaf)) {
    throw "NSP do sysmodule nao encontrado: $sourceNsp"
}
if ((Get-Item -LiteralPath $sourceNsp).Length -lt 4096) {
    throw "O arquivo NSP parece pequeno demais para ser um sysmodule valido."
}

New-Item -ItemType Directory -Path $contents -Force | Out-Null
Copy-Item -LiteralPath $sourceNsp -Destination (Join-Path $contents "exefs.nsp") -Force
if (-not $UpdateOnly) {
    New-Item -ItemType Directory -Path $configDir -Force | Out-Null
    Copy-Item -LiteralPath $configPath -Destination $configDir -Force
}
if ($EnableAutostart -and -not $UpdateOnly) {
    $flags = Join-Path $contents "flags"
    New-Item -ItemType Directory -Path $flags -Force | Out-Null
    New-Item -ItemType File -Path (Join-Path $flags "boot2.flag") -Force | Out-Null
} elseif (Test-Path -LiteralPath (Join-Path $contents "flags\boot2.flag")) {
    Remove-Item -LiteralPath (Join-Path $contents "flags\boot2.flag") -Force
}

$nro = Join-Path $root "switch-richpresence.nro"
if (Test-Path -LiteralPath $nro -PathType Leaf) {
    $nroDir = Join-Path $stage "switch\switch-richpresence"
    New-Item -ItemType Directory -Path $nroDir -Force | Out-Null
    Copy-Item -LiteralPath $nro -Destination (Join-Path $nroDir "switch-richpresence.nro") -Force
}

if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
$entries = @((Join-Path $stage "atmosphere"))
if (-not $UpdateOnly) { $entries += (Join-Path $stage "config") }
if (Test-Path -LiteralPath (Join-Path $stage "switch")) { $entries += (Join-Path $stage "switch") }
Compress-Archive -Path $entries -DestinationPath $zipPath -CompressionLevel Optimal
Write-Output "Pacote criado: $zipPath"
Write-Output "Title ID do sysmodule: $id"
