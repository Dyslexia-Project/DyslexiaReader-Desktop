param(
    [string]$BuildDir = "build",
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [string]$OutputDir = "artifact/DyslexiaReader-Desktop",
    [string]$CommitSha = "local",
    [string]$RefName = "local",
    [switch]$VerifyOcr
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$resolvedBuild = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repoRoot $BuildDir }
$resolvedOutput = if ([System.IO.Path]::IsPathRooted($OutputDir)) { $OutputDir } else { Join-Path $repoRoot $OutputDir }

$configDir = Join-Path $resolvedBuild $Config
$binDir = if (Test-Path (Join-Path $configDir "dyslexia_reader_demo.exe")) { $configDir } else { $resolvedBuild }

$runtimeExes = @("dyslexia_reader_demo.exe", "ocr_cli_demo.exe")
$foundExes = @()
foreach ($exeName in $runtimeExes) {
    $candidate = Join-Path $binDir $exeName
    if (Test-Path $candidate) {
        $foundExes += $candidate
    }
}
if ($foundExes.Count -eq 0) {
    throw "No runtime executables ($($runtimeExes -join ', ')) found in '$binDir'."
}

if (Test-Path $resolvedOutput) {
    Remove-Item -Recurse -Force $resolvedOutput
}
New-Item -ItemType Directory -Force -Path $resolvedOutput | Out-Null

foreach ($exePath in $foundExes) {
    Copy-Item $exePath -Destination $resolvedOutput
    Write-Host "Bundled executable: $(Split-Path -Leaf $exePath)"
}

# Copy runtime DLLs next to the executables (from binDir first, then vcpkg_installed triplet bin).
$vcpkgBinSubdir = if ($Config -eq "Debug") { "debug\bin" } else { "bin" }
$dllSearchDirs = @(
    $binDir,
    (Join-Path $resolvedBuild "vcpkg_installed\x64-windows\$vcpkgBinSubdir")
)
$copiedDlls = @{}
foreach ($dir in $dllSearchDirs) {
    if (-not (Test-Path $dir)) { continue }
    Get-ChildItem -Path $dir -Filter "*.dll" -File | ForEach-Object {
        if (-not $copiedDlls.ContainsKey($_.Name)) {
            Copy-Item $_.FullName -Destination $resolvedOutput
            $copiedDlls[$_.Name] = $true
        }
    }
}
Write-Host "Bundled runtime DLLs: $($copiedDlls.Count)"

# Copy OCR traineddata so Tesseract works from any working directory on a clean PC.
$tessdataSrc = Join-Path $resolvedBuild "tessdata\eng.traineddata"
if (Test-Path $tessdataSrc) {
    $tessdataDest = Join-Path $resolvedOutput "tessdata"
    New-Item -ItemType Directory -Force -Path $tessdataDest | Out-Null
    Copy-Item $tessdataSrc -Destination $tessdataDest
    Write-Host "Bundled OCR model: tessdata\eng.traineddata"
} else {
    Write-Warning "eng.traineddata not found at '$tessdataSrc'."
}

# Copy bundled font assets & license notices.
$fontsSrc = Join-Path $repoRoot "assets\fonts"
if (Test-Path $fontsSrc) {
    $fontsDest = Join-Path $resolvedOutput "fonts"
    New-Item -ItemType Directory -Force -Path $fontsDest | Out-Null
    Copy-Item -Recurse (Join-Path $fontsSrc "*") -Destination $fontsDest
    Write-Host "Bundled font assets from assets\fonts"
}

# Copy shared JSON schema contracts.
$contractsSrc = Join-Path $repoRoot "docs\contracts"
if (Test-Path $contractsSrc) {
    $contractsDest = Join-Path $resolvedOutput "contracts"
    New-Item -ItemType Directory -Force -Path $contractsDest | Out-Null
    Copy-Item -Recurse (Join-Path $contractsSrc "*") -Destination $contractsDest
}

$buildDate = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss UTC")
@"
DyslexiaReader Desktop - Portable Release
=========================================
Commit SHA : $CommitSha
Branch/Ref : $RefName
Build Cfg  : $Config
Build Date : $buildDate
Target Arch: x64 (Win32 unpackaged)
"@ | Set-Content (Join-Path $resolvedOutput "BUILD_INFO.txt")

if ($VerifyOcr) {
    $packagedCli = Join-Path $resolvedOutput "ocr_cli_demo.exe"
    $samplePng = Join-Path $repoRoot "test.png"
    if ((Test-Path $packagedCli) -and (Test-Path $samplePng)) {
        Write-Host "Verifying packaged portable OCR binary from '$env:TEMP'..."
        Push-Location $env:TEMP
        try {
            & $packagedCli $samplePng
            if ($LASTEXITCODE -ne 0) {
                throw "Packaged ocr_cli_demo.exe exited with code $LASTEXITCODE"
            }
        } finally {
            Pop-Location
        }
    }
}
