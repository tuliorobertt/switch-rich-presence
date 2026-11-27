<#
.SYNOPSIS
  Gera todos os arquivos de uma release: pacote do Switch, companion e instalador.

.DESCRIPTION
  1. Compila o NRO e o sysmodule (devkitPro local ou, sem ele, Docker com devkitpro/devkita64).
  2. Monta dist\switch-sd com o layout do cartão SD e gera o zip do Switch.
  3. Compila o companion com PyInstaller e inclui switch-sd dentro dele.
  4. Gera o zip portátil e, se o Inno Setup 6 estiver instalado, o instalador.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File build-release.ps1
  powershell -ExecutionPolicy Bypass -File build-release.ps1 -SkipSwitchBuild
#>
param(
    [switch]$SkipSwitchBuild,   # usa switch-richpresence.nro e o .nsp já compilados
    [switch]$SkipInstaller
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
Set-Location $root
$version = (Select-String -Path (Join-Path $root "Makefile") -Pattern '^export APP_VERSION := (.+)$').Matches[0].Groups[1].Value.Trim()
$dist = Join-Path $root "dist"
$nro = Join-Path $root "switch-richpresence.nro"
$nsp = Join-Path $root "sysmodule\switch-richpresence-sysmodule.nsp"
$guide = Join-Path $root "installer\GUIA-SWITCH.txt"
$moduleId = "4200000000075250"
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

# PowerShell 5.1 turns native stderr (e.g. PyInstaller INFO lines) into errors.
function Invoke-Native([string]$What, [scriptblock]$Command) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Command 2>&1 | ForEach-Object { "$_" } } finally { $ErrorActionPreference = $previous }
    if ($LASTEXITCODE) { throw "Falha: $What (código $LASTEXITCODE)" }
}

# Compress-Archive fails on files that OneDrive or the antivirus hold open;
# reading with FileShare.ReadWrite avoids that. Items: @{ Path; Prefix }.
function New-Zip([string]$Destination, [object[]]$Items) {
    if (Test-Path -LiteralPath $Destination) { Remove-Item -LiteralPath $Destination -Force }
    $zip = [System.IO.Compression.ZipFile]::Open($Destination, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($item in $Items) {
            $base = (Resolve-Path -LiteralPath $item.Path).Path
            if (Test-Path -LiteralPath $base -PathType Leaf) {
                $files = @(Get-Item -LiteralPath $base)
                $parent = Split-Path -Parent $base
            } else {
                $files = @(Get-ChildItem -LiteralPath $base -Recurse -File)
                $parent = $base
            }
            foreach ($file in $files) {
                $relative = $file.FullName.Substring($parent.Length).TrimStart([char]'\').Replace([char]'\', [char]'/')
                $name = if ($item.Prefix) { "$($item.Prefix)/$relative" } else { $relative }
                $entry = $zip.CreateEntry($name, [System.IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = $file.LastWriteTime
                $in = [System.IO.File]::Open($file.FullName, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                                             [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
                $out = $entry.Open()
                try { $in.CopyTo($out) } finally { $out.Dispose(); $in.Dispose() }
            }
        }
    } finally { $zip.Dispose() }
}

Write-Output "== Switch Rich Presence $version =="

# 1. Switch ---------------------------------------------------------------
if (-not $SkipSwitchBuild) {
    if ($env:DEVKITPRO) {
        Write-Output "Compilando com devkitPro local..."
        Invoke-Native "compilar o NRO" { make }
        Invoke-Native "compilar o sysmodule" { make -C sysmodule }
    } elseif (Get-Command docker -ErrorAction SilentlyContinue) {
        Write-Output "Compilando com Docker (devkitpro/devkita64)..."
        Invoke-Native "compilar no Docker" { docker run --rm -v "${root}:/src" -w /src devkitpro/devkita64 sh -c "make && make -C sysmodule" }
    } else {
        throw "Instale o devkitPro ou o Docker, ou use -SkipSwitchBuild com os binários já compilados."
    }
}
foreach ($file in @($nro, $nsp)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Arquivo não encontrado: $file" }
}

$sd = Join-Path $dist "switch-sd"
if (Test-Path $sd) { Remove-Item -Recurse -Force $sd }
$contents = Join-Path $sd "atmosphere\contents\$moduleId"
New-Item -ItemType Directory -Force -Path (Join-Path $contents "flags"), (Join-Path $sd "config"), (Join-Path $sd "switch\switch-richpresence") | Out-Null
Copy-Item $nsp (Join-Path $contents "exefs.nsp")
New-Item -ItemType File -Force -Path (Join-Path $contents "flags\boot2.flag") | Out-Null
Copy-Item (Join-Path $root "sysmodule\config\switch-richpresence.ini") (Join-Path $sd "config")
Copy-Item $nro (Join-Path $sd "switch\switch-richpresence\switch-richpresence.nro")

$switchZip = Join-Path $dist "SwitchRichPresence-Switch-$version.zip"
New-Zip $switchZip @(@{ Path = $sd; Prefix = "" }, @{ Path = $guide; Prefix = "" })
Write-Output "Pacote do Switch: $switchZip"

# 2. Companion ------------------------------------------------------------
Write-Output "Compilando o companion (PyInstaller)..."
Invoke-Native "PyInstaller" { python -m PyInstaller --noconfirm --log-level WARN SwitchRichPresence.spec }
$app = Join-Path $dist "SwitchRichPresence"
Copy-Item -Recurse -Force $sd (Join-Path $app "switch-sd")
Copy-Item -Force $guide $app

$portable = Join-Path $dist "SwitchRichPresence-Windows-$version.zip"
New-Zip $portable @(@{ Path = $app; Prefix = "SwitchRichPresence" })
Write-Output "Companion portátil: $portable"

# 3. Instalador -----------------------------------------------------------
if (-not $SkipInstaller) {
    $iscc = @(
        (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source,
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    ) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if ($iscc) {
        Invoke-Native "gerar o instalador" { & $iscc "/DAppVersion=$version" (Join-Path $root "installer\SwitchRichPresence.iss") }
        Write-Output "Instalador: $(Join-Path $dist "SwitchRichPresence-Setup-$version.exe")"
    } else {
        Write-Warning "Inno Setup 6 não encontrado; instalador não gerado. Instale com: winget install JRSoftware.InnoSetup"
    }
}
