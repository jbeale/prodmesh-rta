# Builds ProdMesh Remote RTA on Windows using the Qt online-installer layout
# (C:\Qt\<version>\mingw_64 + C:\Qt\Tools). Run via build.bat or:
#   powershell -ExecutionPolicy Bypass -File build.ps1
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# --- locate the newest installed Qt MinGW kit ---
$qtRoot = if ($env:QTDIR) { Split-Path (Split-Path $env:QTDIR) } else { "C:\Qt" }
$kits = Get-ChildItem -Path $qtRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^6\.' -and (Test-Path "$($_.FullName)\mingw_64") } |
    Sort-Object { [version]$_.Name } -Descending
if (-not $kits) {
    Write-Host "No Qt 6 MinGW kit found under $qtRoot." -ForegroundColor Red
    Write-Host "Install Qt from https://www.qt.io/download-qt-installer and select:"
    Write-Host "  - Qt 6.x 'Desktop' with the MinGW kit"
    Write-Host "  - Additional Libraries: Qt Multimedia"
    Write-Host "  - Build Tools: CMake, Ninja, MinGW"
    exit 1
}
$kit = "$($kits[0].FullName)\mingw_64"

$mingw = Get-ChildItem -Path "$qtRoot\Tools" -Directory -Filter "mingw*_64" |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $mingw) { Write-Host "MinGW not found in $qtRoot\Tools" -ForegroundColor Red; exit 1 }

$env:PATH = "$($mingw.FullName)\bin;$qtRoot\Tools\CMake_64\bin;$qtRoot\Tools\Ninja;$kit\bin;$env:PATH"
Write-Host "Using Qt kit: $kit"

# --- configure + build ---
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH="$kit" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit 1 }
cmake --build build
if ($LASTEXITCODE -ne 0) { exit 1 }

# --- bundle Qt DLLs next to the exe so it runs anywhere ---
windeployqt --release "build\ProdMeshRemoteRTA.exe" | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "windeployqt failed" -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "Build complete: build\ProdMeshRemoteRTA.exe" -ForegroundColor Green
Write-Host "The build folder is self-contained - copy it to any Windows PC."
