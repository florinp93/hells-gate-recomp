param(
    [string]$DiligentDir = "thirdparty\diligent-core"
)

$target = Join-Path (Split-Path -Parent $PSScriptRoot) $DiligentDir
$patch = Join-Path $PSScriptRoot "diligent\diligent-core.patch"

if (-not (Test-Path $target)) {
    Write-Error "DiligentCore not found at $target"
    exit 1
}

git -C $target apply --reverse --check $patch 2>$null
if ($LASTEXITCODE -eq 0) {
    Write-Host "DiligentCore patches already applied."
    exit 0
}

git -C $target apply $patch
if ($LASTEXITCODE -ne 0) { git -C $target apply --3way $patch }
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to apply $patch"
    exit 1
}
Write-Host "DiligentCore patches applied."
