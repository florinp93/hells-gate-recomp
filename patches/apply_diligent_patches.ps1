param(
    [string]$DiligentDir = "thirdparty\diligent-core"
)

$ErrorActionPreference = "Continue"
$PSNativeCommandUseErrorActionPreference = $false

$projectRoot = Split-Path -Parent $PSScriptRoot
$diligentPath = Join-Path $projectRoot $DiligentDir
$patchFile = Join-Path $PSScriptRoot "diligent\diligent-core.patch"

if (-not (Test-Path $diligentPath)) {
    Write-Error "DiligentCore directory not found: $diligentPath"
    exit 1
}

if (-not (Test-Path $patchFile)) {
    Write-Error "Patch file not found: $patchFile"
    exit 1
}

Write-Host "Applying DiligentCore patches to $diligentPath ..."
Push-Location $diligentPath
try {
    $reverseCheck = & git apply --reverse --check $patchFile 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "DiligentCore patches already applied."
        Pop-Location
        exit 0
    }

    $applyResult = & git apply $patchFile 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Attempting to apply with --3way..."
        $applyResult = & git apply --3way $patchFile 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Failed to apply DiligentCore patches: $applyResult"
            Pop-Location
            exit 1
        }
    }
    Write-Host "DiligentCore patches applied successfully."
}
finally {
    Pop-Location
}
