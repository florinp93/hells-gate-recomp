param(
    [string]$Version,
    [string]$GameBuildDir = "..\out\build\win-amd64-release",
    [string]$Configuration = "Release",
    [string]$OutputDir = "..\beta-release"
)

$ErrorActionPreference = "Stop"

$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $msbuild)) {
    Write-Error "MSBuild not found. Install Visual Studio 2022 Community or update the path."
}

$iscc = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) {
    Write-Error "Inno Setup 6 not found at $iscc. Install it via winget: winget install JRSoftware.InnoSetup"
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$GameBuildDir = (Join-Path $root $GameBuildDir | Resolve-Path -ErrorAction Stop).Path
$OutputDir = [System.IO.Path]::GetFullPath((Join-Path $root $OutputDir))

$sourceVersionFile = Join-Path $root "DantesInfernoLauncher\version.txt"
$issFile = Join-Path $root "installer.iss"

if (-not $Version) {
    $Version = (Get-Content $sourceVersionFile -Raw).Trim()
}
Write-Host "Version: $Version"

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($sourceVersionFile, $Version + "`r`n", $utf8NoBom)

$issContent = [System.IO.File]::ReadAllText($issFile, [System.Text.Encoding]::UTF8)
$issContent = [System.Text.RegularExpressions.Regex]::Replace(
    $issContent, '#define MyAppVersion\s+"[^"]*"', "#define MyAppVersion ""$Version""")
[System.IO.File]::WriteAllText($issFile, $issContent, $utf8NoBom)

$gameFiles = @("dantes_inferno.exe", "rexruntime.dll", "rexgpu-xenos.dll", "amd_fidelityfx_dx12.dll")
foreach ($f in $gameFiles) {
    if (-not (Test-Path (Join-Path $GameBuildDir $f))) { Write-Error "Missing game binary: $f in $GameBuildDir" }
}

# Releases never carry the development cheats (CMake DANTESINFERNO_DEV_CHEATS).
$exeText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes((Join-Path $GameBuildDir "dantes_inferno.exe")))
if ($exeText.Contains("dante_cheat_god")) {
    Write-Error "dantes_inferno.exe was built with DANTESINFERNO_DEV_CHEATS. Reconfigure with -DDANTESINFERNO_DEV_CHEATS=OFF and rebuild."
}

$ffxVkDll = Join-Path $GameBuildDir "thirdparty\rexglue-sdk\out\win-amd64\amd_fidelityfx_vk.dll"

$tuSrc = Join-Path $root "..\game\default.xexp"
$tuDst = Join-Path $GameBuildDir "default.xexp"
if (Test-Path $tuSrc) {
    Copy-Item $tuSrc $tuDst -Force
} elseif (-not (Test-Path $tuDst)) {
    Write-Error "default.xexp (TU2) not found at $tuSrc or in $GameBuildDir - the game cannot start without it."
}

$extractXiso = Join-Path $root "..\tools\extract-xiso\extract-xiso.exe"
if (-not (Test-Path $extractXiso)) {
    Write-Error "Missing extract-xiso.exe at $extractXiso"
}

Write-Host "Building C# solution..."
& $msbuild (Join-Path $root "DantesInferno.sln") /p:Configuration=$Configuration /p:Platform=AnyCPU /v:minimal
if ($LASTEXITCODE -ne 0) { throw "C# build failed." }

$binVersionFile = Join-Path $root "DantesInfernoLauncher\bin\$Configuration\version.txt"
[System.IO.File]::WriteAllText($binVersionFile, $Version + "`r`n", $utf8NoBom)

if (Test-Path $OutputDir) { Remove-Item $OutputDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

Write-Host "Compiling Inno Setup installer..."
& $iscc "/O$OutputDir" "/DGameBuildDir=$GameBuildDir" (Join-Path $root "installer.iss") 2>&1
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed." }

# Portable zip: the installed files without an installer (no admin rights);
# portable.txt keeps saves next to the game and the launcher imports the ISO.
$zipName = "DantesInferno-$Version-portable.zip"
$staging = Join-Path ([System.IO.Path]::GetTempPath()) "DantesInfernoPortable-$Version"
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $staging "game") -Force | Out-Null
foreach ($f in $gameFiles) { Copy-Item (Join-Path $GameBuildDir $f) $staging }
if (Test-Path $ffxVkDll) { Copy-Item $ffxVkDll $staging }
$launcherBin = Join-Path $root "DantesInfernoLauncher\bin\$Configuration"
foreach ($f in @("DantesInfernoLauncher.exe", "DantesInferno.Shared.dll", "version.txt")) {
    Copy-Item (Join-Path $launcherBin $f) $staging
}
Copy-Item (Join-Path $root "icon.ico"), (Join-Path $root "banner.jpg"), $extractXiso $staging
New-Item -ItemType Directory -Path (Join-Path $staging "tu") -Force | Out-Null
Copy-Item $tuDst (Join-Path $staging "game")
Copy-Item $tuDst (Join-Path $staging "tu")
$shaderList = Join-Path $root "..\shader-list\dante_pipelines.bin"
if (Test-Path $shaderList) {
    Copy-Item $shaderList $staging
} else {
    Write-Warning "shader-list\dante_pipelines.bin not found - this release has no shader precompile list."
}
[System.IO.File]::WriteAllText((Join-Path $staging "portable.txt"),
    "Portable mode: saves and settings are kept in the userdata folder next to this file.`r`n" +
    "Delete this file (or untick Portable mode in the launcher) to use Documents again.`r`n", $utf8NoBom)
# Windows tar writes standard zip paths (Compress-Archive in PowerShell 5.1 writes
# backslashes, which other unzip tools keep in the file name).
& (Join-Path $env:SystemRoot "System32\tar.exe") -a -c -f (Join-Path $OutputDir $zipName) -C $staging (Get-ChildItem $staging).Name
if ($LASTEXITCODE -ne 0) { throw "Creating $zipName failed." }
Remove-Item $staging -Recurse -Force
Write-Host "Portable zip created at: $OutputDir\$zipName"

# Debug symbols of this release (not shipped): map crash-log addresses to functions.
$symbolsDir = [System.IO.Path]::GetFullPath((Join-Path $root "..\release-symbols\$Version"))
New-Item -ItemType Directory -Path $symbolsDir -Force | Out-Null
foreach ($f in @("dantes_inferno.exe", "dantes_inferno.pdb")) {
    $src = Join-Path $GameBuildDir $f
    if (Test-Path $src) {
        Copy-Item $src $symbolsDir -Force
    } else {
        Write-Warning "Missing $src - crash addresses of this release cannot be symbolized."
    }
}
Write-Host "Symbols archived in: $symbolsDir"

$installerExe = Join-Path $OutputDir "DantesInfernoInstaller.exe"
Write-Host ("Installer: {0} ({1:N1} MB)" -f $installerExe, ((Get-Item $installerExe).Length / 1MB))
