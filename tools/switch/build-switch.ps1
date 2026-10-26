# Builds reblue.nro on Windows with Docker Desktop. Two stages, each in its own
# image (see docs/SWITCH.md):
#   1. host stage   reblue-switch-host (tools/switch/Dockerfile.host, built here
#                   the first time): code generation and the shader cache
#   2. Switch stage the switch-dev image (devkitA64, libnx, mesa-switch NVK):
#                   the cross build and the NRO
#
#   powershell -ExecutionPolicy Bypass -File tools\switch\build-switch.ps1
#   powershell -ExecutionPolicy Bypass -File tools\switch\build-switch.ps1 -Game D:\Games\reblue\game -Jobs 4 -Clean
#
# -Game is the game folder of a re:Blue PC install (default.xex, pack\, movie\...).
# By default the folder "game" next to the repository is used.
param(
    [string]$Game = "",
    [string]$SwitchImage = "",
    [string]$HostImage = "reblue-switch-host",
    [int]$Jobs = 0,
    [ValidateSet("Release", "RelWithDebInfo")][string]$BuildType = "Release",
    [switch]$Clean,
    # Link-time optimisation of the game code: faster game, slow link, ~16 GB of RAM for Docker.
    [switch]$Lto,
    # Plain calls between recompiled functions (undoes tools/switch/direct_calls.py).
    [switch]$NoDirectCalls,
    # Also the GPU test NRO (tools/switch/gputest, docs/SWITCH-deko3d.md P0.4).
    [switch]$GpuTest,
    # Leave the deko3d back end out (NVK only). With it, bd_switch_backend picks deko3d or nvk at start.
    [switch]$NoDeko3d
)
# Continue, not Stop: Windows PowerShell 5.1 turns a native program's redirected
# stderr line into a terminating error. Every docker call checks its exit code.
$ErrorActionPreference = "Continue"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

function Invoke-Docker {
    param([string[]]$DockerArgs)
    & docker @DockerArgs
    if ($LASTEXITCODE -ne 0) { throw "docker $($DockerArgs[0]) failed (exit $LASTEXITCODE)" }
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw "Docker is not installed or not on PATH. Install Docker Desktop (WSL 2 backend)."
}
& docker info *> $null
if ($LASTEXITCODE -ne 0) { throw "Docker Desktop is not running. Start it and try again." }

# ---------------------------------------------------------------- game folder
if (-not $Game) { $Game = Join-Path (Split-Path $root -Parent) "game" }
$haveAssets = (Test-Path (Join-Path $root "assets\default.xex")) -and (Test-Path (Join-Path $root "assets\shaders"))
$gameMount = @()
if (Test-Path (Join-Path $Game "default.xex")) {
    $Game = (Resolve-Path $Game).Path
    Write-Host "Game folder: $Game"
    $gameMount = @("-v", "${Game}:/game:ro")
} elseif (-not $haveAssets) {
    throw "No default.xex in '$Game'. Pass the game folder of a re:Blue install with -Game <folder>."
}

# ---------------------------------------------------------------- images
& docker image inspect $HostImage *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Building the host tools image $HostImage (first run only)..."
    Invoke-Docker @("build", "--platform", "linux/amd64", "-t", $HostImage,
        "-f", (Join-Path $root "tools\switch\Dockerfile.host"), (Join-Path $root "tools\switch"))
}
if (-not $SwitchImage) {
    # The image the Lost Odyssey port built (run-switch-build.bat there), else
    # the published one.
    & docker image inspect lo-switch-dev *> $null
    if ($LASTEXITCODE -eq 0) { $SwitchImage = "lo-switch-dev" }
    else { $SwitchImage = "ghcr.io/autorunhq/switch-dev:2026.10.05" }
}
& docker image inspect $SwitchImage *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Pulling the Switch toolchain image $SwitchImage (first run only, a few GB)..."
    Invoke-Docker @("pull", $SwitchImage)
}
Write-Host "Switch image: $SwitchImage"

$envArgs = @("-e", "BUILD_TYPE=$BuildType")
if ($Jobs -gt 0) { $envArgs += @("-e", "JOBS=$Jobs") }
if ($Clean) { $envArgs += @("-e", "CLEAN=1") }
if ($Lto) { $envArgs += @("-e", "LTO=1") }
if ($NoDirectCalls) { $envArgs += @("-e", "DIRECT_CALLS=0") }
if ($GpuTest) { $envArgs += @("-e", "GPUTEST=1") }
if ($NoDeko3d) { $envArgs += @("-e", "DEKO3D=0") } else { $envArgs += @("-e", "DEKO3D=1") }

# ---------------------------------------------------------------- stages
Write-Host ""
Write-Host "==== Stage 1: host tools, codegen, shader cache ===="
Invoke-Docker (@("run", "--rm") + $envArgs + @("-v", "${root}:/work") + $gameMount +
    @("-w", "/work", $HostImage, "bash", "tools/switch/host-stage.sh"))

Write-Host ""
Write-Host "==== Stage 2: Switch build ===="
Invoke-Docker (@("run", "--rm") + $envArgs + @("-v", "${root}:/work", "-w", "/work",
    $SwitchImage, "bash", "tools/switch/build-switch.sh"))

Write-Host ""
Write-Host "Copy out\switch\reblue.nro to sdmc:/switch/reblue/ on the SD card."
