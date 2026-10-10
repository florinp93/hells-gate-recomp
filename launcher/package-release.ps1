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
    if (Test-Path $sourceVersionFile) {
        $Version = (Get-Content $sourceVersionFile -Raw).Trim()
    }
    if (-not $Version) {
        $Version = "0.0.0"
    }
    Write-Host "Using version from version.txt: $Version"
} else {
    Write-Host "Updating version to: $Version"
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($sourceVersionFile, $Version + "`r`n", $utf8NoBom)

if (Test-Path $issFile) {
    $issContent = [System.IO.File]::ReadAllText($issFile, [System.Text.Encoding]::UTF8)
    $issContent = [System.Text.RegularExpressions.Regex]::Replace(
        $issContent,
        '#define MyAppVersion\s+"[^"]*"',
        "#define MyAppVersion ""$Version""")
    [System.IO.File]::WriteAllText($issFile, $issContent, $utf8NoBom)
    Write-Host "Updated installer.iss MyAppVersion -> $Version"
}

$gameFiles = @("dantes_inferno.exe", "rexruntime.dll", "rexgpu-xenos.dll", "amd_fidelityfx_dx12.dll")
foreach ($f in $gameFiles) {
    $src = Join-Path $GameBuildDir $f
    if (-not (Test-Path $src)) {
        Write-Error "Missing game binary: $src"
    }
}

$ffxVkDll = Join-Path $GameBuildDir "thirdparty\rexglue-sdk\out\win-amd64\amd_fidelityfx_vk.dll"
if (Test-Path $ffxVkDll) {
    Write-Host "Found amd_fidelityfx_vk.dll at $ffxVkDll"
} else {
    Write-Host "amd_fidelityfx_vk.dll not built; installer will skip it (not required for Vulkan CAS/FSR shaders)"
}

$tuSrc = Join-Path $root "..\game\default.xexp"
$tuDst = Join-Path $GameBuildDir "default.xexp"
if (Test-Path $tuSrc) {
    Copy-Item $tuSrc $tuDst -Force
    Write-Host "Copied default.xexp (TU2 patch) to build output"
} else {
    Write-Warning "default.xexp not found at $tuSrc - TU2 will not be bundled with the installer."
}

$extractXiso = Join-Path $root "..\tools\extract-xiso\extract-xiso.exe"
if (-not (Test-Path $extractXiso)) {
    Write-Error "Missing extract-xiso.exe at $extractXiso"
}

Write-Host "Building C# solution..."
& $msbuild (Join-Path $root "DantesInferno.sln") /p:Configuration=$Configuration /p:Platform=AnyCPU /v:minimal
if ($LASTEXITCODE -ne 0) { throw "C# build failed." }

$binVersionFile = Join-Path $root "DantesInfernoLauncher\bin\$Configuration\version.txt"
if (Test-Path $binVersionFile) {
    $binVersion = (Get-Content $binVersionFile -Raw).Trim()
    if ($binVersion -ne $Version) {
        Write-Warning "version.txt in bin output ($binVersion) does not match expected ($Version) - overwriting."
        [System.IO.File]::WriteAllText($binVersionFile, $Version + "`r`n", $utf8NoBom)
    } else {
        Write-Host "version.txt in build output verified: $binVersion"
    }
} else {
    Write-Warning "version.txt not found in launcher bin - creating with version $Version."
    [System.IO.File]::WriteAllText($binVersionFile, $Version + "`r`n", $utf8NoBom)
}

if (Test-Path $OutputDir) { Remove-Item $OutputDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

Write-Host "Compiling Inno Setup installer..."
& $iscc (Join-Path $root "installer.iss") 2>&1
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed." }

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

Write-Host ""
Write-Host "Installer created at: $OutputDir\DantesInfernoInstaller.exe"
$installerExe = Join-Path $OutputDir "DantesInfernoInstaller.exe"
if (Test-Path $installerExe) {
    $size = (Get-Item $installerExe).Length
    $sizeMB = [math]::Round($size / 1048576, 2)
    Write-Host "Size: $sizeMB MB"
    Write-Host "Version: $Version"
}
