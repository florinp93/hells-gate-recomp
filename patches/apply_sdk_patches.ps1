param(
    [string]$SdkDir = "thirdparty\rexglue-sdk"
)

$target = Join-Path (Split-Path -Parent $PSScriptRoot) $SdkDir
$patch = Join-Path $PSScriptRoot "sdk\rexglue-sdk-v0.10.0.patch"

if (-not (Test-Path $target)) {
    Write-Error "SDK not found at $target. Run setup.ps1 first."
    exit 1
}

git -C $target apply --reverse --check $patch 2>$null
if ($LASTEXITCODE -eq 0) {
    Write-Host "SDK patches already applied."
    exit 0
}

git -C $target apply $patch
if ($LASTEXITCODE -ne 0) { git -C $target apply --3way $patch }
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to apply $patch"
    exit 1
}
Write-Host "SDK patches applied."
