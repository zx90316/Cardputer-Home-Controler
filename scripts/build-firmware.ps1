param(
    [switch]$RunNativeTests
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$mappedDrive = $null
$buildRoot = $root

if ($root -match '[^\x00-\x7F]') {
    $used = @(Get-PSDrive -PSProvider FileSystem | ForEach-Object Name)
    $letter = @('P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z') |
        Where-Object { $_ -notin $used } | Select-Object -First 1
    if (-not $letter) { throw "No free drive letter is available for the ASCII build path." }
    $mappedDrive = "${letter}:"
    & subst $mappedDrive $root
    if ($LASTEXITCODE -ne 0) { throw "Unable to create temporary build drive $mappedDrive." }
    $buildRoot = "$mappedDrive\"
}

try {
    $python = Join-Path $buildRoot ".venv\Scripts\python.exe"
    $pio = Join-Path $buildRoot ".venv\Scripts\pio.exe"
    $out = Join-Path $buildRoot "firmware\build"
    New-Item -ItemType Directory -Force $out | Out-Null
    $existingApp = Join-Path $out "cardputer-home-controller-app.bin"
    $previousApp = Join-Path $out "cardputer-home-controller-app-previous.bin"
    if ((Test-Path $existingApp) -and -not (Test-Path $previousApp)) {
        Copy-Item $existingApp $previousApp
    }
    if (-not (Test-Path $pio)) {
        & $python -m pip install --requirement (Join-Path $buildRoot "firmware\requirements.txt")
    }
    $env:PLATFORMIO_CORE_DIR = Join-Path $buildRoot ".platformio"
    & $pio run -d (Join-Path $buildRoot "firmware")
    if ($LASTEXITCODE -ne 0) { throw "Firmware build failed." }

    if ($RunNativeTests) {
        if (-not (Test-Path (Join-Path $env:PLATFORMIO_CORE_DIR "packages\toolchain-gccmingw32\bin\gcc.exe"))) {
            & $pio pkg install --global --tool platformio/toolchain-gccmingw32
        }
        $env:PATH = (Join-Path $env:PLATFORMIO_CORE_DIR "packages\toolchain-gccmingw32\bin") + ";" + $env:PATH
        & $pio test -d (Join-Path $buildRoot "firmware") -e native
        if ($LASTEXITCODE -ne 0) { throw "Native tests failed." }
    }

    $build = Join-Path $buildRoot "firmware\.pio\build\cardputer-adv"
    $esptool = Join-Path $env:PLATFORMIO_CORE_DIR "packages\tool-esptoolpy\esptool.py"
    $bootApp = Join-Path $env:PLATFORMIO_CORE_DIR "packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"
    & $python $esptool --chip esp32s3 merge_bin --flash_mode keep --flash_freq 80m --flash_size 8MB `
        -o (Join-Path $out "cardputer-home-controller-complete.bin") `
        0x0 (Join-Path $build "bootloader.bin") `
        0x8000 (Join-Path $build "partitions.bin") `
        0xe000 $bootApp `
        0x10000 (Join-Path $build "firmware.bin")
    if ($LASTEXITCODE -ne 0) { throw "Merged binary generation failed." }
    Copy-Item -Force (Join-Path $build "firmware.bin") (Join-Path $out "cardputer-home-controller-app.bin")
    $version = (Get-Content -Raw (Join-Path $buildRoot "VERSION")).Trim()
    $appPath = Join-Path $out "cardputer-home-controller-app.bin"
    $completePath = Join-Path $out "cardputer-home-controller-complete.bin"
    $manifest = [ordered]@{
        version = $version
        channel = "release-candidate"
        hardware_validated = $false
        generated_utc = [DateTime]::UtcNow.ToString("o")
        artifacts = @(
            [ordered]@{ name = [IO.Path]::GetFileName($appPath); offset = "0x10000"; size = (Get-Item $appPath).Length; sha256 = (Get-FileHash $appPath -Algorithm SHA256).Hash },
            [ordered]@{ name = [IO.Path]::GetFileName($completePath); offset = "0x0"; size = (Get-Item $completePath).Length; sha256 = (Get-FileHash $completePath -Algorithm SHA256).Hash }
        )
    }
    $manifest | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $out "firmware-manifest.json")
    Write-Host "Firmware artifacts: $out"
} finally {
    if ($mappedDrive) { & subst $mappedDrive /D }
}
