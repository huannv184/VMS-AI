# Fetch the OSNet x1.0 ONNX model and place it where ReIDEngine looks
# for it on startup (cpp-backend\models\osnet_x1_0.onnx). Required to
# move the cross-camera Re-ID feature off its color-histogram fallback
# onto real OSNet embeddings.
#
# Without the file present:
#   - ReIDEngine::init() falls through all four candidate paths and
#     logs "No ONNX model found. Running in hash-based fallback mode
#     (reduced accuracy)." (cpp-backend\src\core\reid_engine.cpp:132)
#   - The engine still works — gallery + trails + persistence (the
#     2026-05-14 reid_persistence pass) all function — but match
#     accuracy is significantly lower because the embedding is a
#     128-bin color histogram, not a learned 512-dim feature vector.
#
# Where to obtain a valid OSNet x1.0 ONNX:
#   1. Self-export from torchreid (https://kaiyangzhou.github.io/deep-person-reid/):
#        pip install torchreid
#        # Then in Python:
#        #   import torchreid, torch
#        #   m = torchreid.models.build_model('osnet_x1_0', num_classes=1000, pretrained=True)
#        #   m.eval()
#        #   torch.onnx.export(m, torch.randn(1,3,256,128), 'osnet_x1_0.onnx',
#        #                     input_names=['input'], output_names=['features'],
#        #                     dynamic_axes={'input':{0:'batch'}, 'features':{0:'batch'}},
#        #                     opset_version=11)
#      This produces the canonical (1,3,256,128)->(1,512) network the
#      backend expects (resize/blob path in reid_engine.cpp:395 uses
#      cv::Size(128, 256), matching the H=256/W=128 OSNet input).
#   2. A community pre-export (e.g. yolo_tracking releases). Trust the
#      upstream maintainer; pin via -Sha256 below.
#   3. An internal artifact server. Pass -Url <https url> + -Sha256.
#
# Usage:
#   $env:OSNET_MODEL_URL = "https://example.com/osnet_x1_0.onnx"
#   $env:OSNET_MODEL_SHA256 = "abc123..."   # optional but recommended
#   .\scripts\deploy_osnet_model.ps1
#
#   # or with explicit args:
#   .\scripts\deploy_osnet_model.ps1 -Url "https://..." -Sha256 "abc..."
#
#   # force re-download even if file already exists and matches:
#   .\scripts\deploy_osnet_model.ps1 -Force
#
# Idempotent: if the target file exists and (a) no -Sha256 was given,
# or (b) the existing file matches the given -Sha256, the script
# returns successfully without re-downloading. No service restart is
# triggered by this script — the model is loaded once at backend boot,
# so the operator restarts the service (e.g. NSSM stop+start) after
# deploying a new model.

param(
    [string]$Url       = $env:OSNET_MODEL_URL,
    [string]$Sha256    = $env:OSNET_MODEL_SHA256,
    [switch]$Force,
    [string]$ModelName = 'osnet_x1_0.onnx'
)

$ErrorActionPreference = 'Stop'

$RepoRoot  = Split-Path -Parent $PSScriptRoot
$ModelsDir = Join-Path $RepoRoot 'cpp-backend\models'
$Target    = Join-Path $ModelsDir $ModelName

function Get-FileSha256Lower([string]$Path) {
    return (Get-FileHash -Path $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Test-ExistingMatches {
    if (-not (Test-Path $Target)) { return $false }
    if ($Force) {
        Write-Host "[osnet-deploy] -Force given; ignoring existing file at $Target"
        return $false
    }
    if ([string]::IsNullOrWhiteSpace($Sha256)) {
        Write-Host "[osnet-deploy] $Target already exists ($([math]::Round((Get-Item $Target).Length/1MB,2)) MB). No -Sha256 given, skipping re-download."
        Write-Host "[osnet-deploy] Pass -Force to re-download or -Sha256 to verify integrity."
        return $true
    }
    $actual = Get-FileSha256Lower $Target
    $expected = $Sha256.ToLowerInvariant()
    if ($actual -eq $expected) {
        Write-Host "[osnet-deploy] $Target already present with matching sha256, nothing to do."
        return $true
    }
    Write-Host "[osnet-deploy] Existing file sha256 mismatch (have $actual, want $expected). Will re-download."
    return $false
}

# ── 1. Validate inputs ──────────────────────────────────────────────────
if ([string]::IsNullOrWhiteSpace($Url) -and -not (Test-Path $Target)) {
    Write-Error @"
No -Url / `$env:OSNET_MODEL_URL provided and no existing model at $Target.
See the header of this script for where to obtain the OSNet x1.0 ONNX.
"@
    exit 1
}

if (-not (Test-Path $ModelsDir)) {
    Write-Host "[osnet-deploy] Creating $ModelsDir"
    New-Item -ItemType Directory -Path $ModelsDir -Force | Out-Null
}

# ── 2. Idempotency short-circuit ────────────────────────────────────────
if (Test-ExistingMatches) {
    exit 0
}

if ([string]::IsNullOrWhiteSpace($Url)) {
    Write-Error "Existing model failed integrity check but no -Url given to re-download."
    exit 1
}

# ── 3. Download to temp + atomic move ───────────────────────────────────
# Atomic move: avoids leaving a half-downloaded file at $Target if the
# download is interrupted (Ctrl+C, network drop). Re-running the script
# then finds no model and starts fresh.
$Tmp = Join-Path $env:TEMP "osnet_x1_0.$([guid]::NewGuid().ToString('N')).onnx.part"

try {
    Write-Host "[osnet-deploy] Downloading from $Url"
    Write-Host "[osnet-deploy] Target: $Target"

    # Use System.Net.WebClient for a real progress indicator on older
    # PowerShell. Invoke-WebRequest with -OutFile in PS 5.1 is fine but
    # spams a progress bar that can hide errors.
    $prevProgress = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $Url -OutFile $Tmp -UseBasicParsing
    } finally {
        $ProgressPreference = $prevProgress
    }

    # ── 4. Sanity-check size ────────────────────────────────────────────
    # OSNet x1.0 ONNX is consistently ~10 MB. Reject anything wildly off
    # (HTML error page, truncated download, wrong model).
    $sizeBytes = (Get-Item $Tmp).Length
    $sizeMB    = [math]::Round($sizeBytes / 1MB, 2)
    if ($sizeBytes -lt 1MB) {
        throw "Downloaded file is only $sizeMB MB (< 1 MB). Likely an HTML error page or wrong URL. Aborting."
    }
    if ($sizeBytes -gt 100MB) {
        throw "Downloaded file is $sizeMB MB (> 100 MB). OSNet x1.0 is ~10 MB; this is probably the wrong model. Aborting."
    }
    Write-Host "[osnet-deploy] Downloaded $sizeMB MB"

    # ── 5. Sanity-check format ──────────────────────────────────────────
    # ONNX files start with the protobuf tag for the IR version field.
    # We don't fully parse protobuf here; just reject anything that looks
    # like HTML (the most common "I got an error page instead of a file"
    # failure mode).
    $head = [System.IO.File]::ReadAllBytes($Tmp) | Select-Object -First 64
    $headStr = [System.Text.Encoding]::ASCII.GetString($head)
    if ($headStr -match '^\s*(<!DOCTYPE|<html|<\?xml)') {
        throw "Downloaded file looks like HTML/XML, not a binary model. Check the URL. First bytes: '$($headStr.Substring(0, [Math]::Min(40, $headStr.Length)))'"
    }

    # ── 6. Optional sha256 verification ─────────────────────────────────
    if (-not [string]::IsNullOrWhiteSpace($Sha256)) {
        $actual = Get-FileSha256Lower $Tmp
        $expected = $Sha256.ToLowerInvariant()
        if ($actual -ne $expected) {
            throw "sha256 mismatch: got $actual, expected $expected. Aborting (file NOT installed)."
        }
        Write-Host "[osnet-deploy] sha256 verified: $actual"
    } else {
        Write-Host "[osnet-deploy] No -Sha256 given; skipping integrity check (recommended for production)."
    }

    # ── 7. Atomic move into place ───────────────────────────────────────
    Move-Item -Path $Tmp -Destination $Target -Force
    Write-Host "[osnet-deploy] Installed at $Target"
    Write-Host "[osnet-deploy] Restart vms_backend (e.g. NSSM stop+start) for ReIDEngine to pick it up."
}
catch {
    Write-Error $_
    if (Test-Path $Tmp) { Remove-Item $Tmp -Force -ErrorAction SilentlyContinue }
    exit 1
}
