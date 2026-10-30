# Downloads a track's full public dataset (seed 1, the board's data) into
# data/<track>-full and copies it into the Docker volume sd-<track>, which is
# what bench.ps1 and cmp.ps1 mount. Reading from a volume avoids Docker
# Desktop's slow Windows file share, so timings are much less noisy.
#
#   powershell -File tools\get-data.ps1 -Track a
param([Parameter(Mandatory = $true)][ValidateSet("a", "b", "c", "d")][string]$Track)

$files = @{ a = @("requests.log"); b = @("docs.txt", "queries.txt");
            c = @("records.csv", "regions.csv"); d = @("graph.txt", "queries.txt") }
$base = "https://s3.amazonaws.com/raptors.dev/speeddemon.dev/2026/data/track-$Track/full-seed1"
$root = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $root "data\$Track-full"
New-Item -ItemType Directory -Force $dir | Out-Null

foreach ($f in $files[$Track]) {
    $dest = Join-Path $dir $f
    if (-not (Test-Path $dest)) {
        Write-Host "download $f"
        curl.exe -s -o $dest "$base/$f"
    }
    $h = (Get-FileHash -Algorithm SHA256 $dest).Hash.ToLower()
    Write-Host "sha256  $f  $h"
}

$vol = "sd-$Track"
docker volume create $vol | Out-Null
docker run --rm -v "${dir}:/src:ro" -v "${vol}:/data" `
    debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad `
    sh -c "cp /src/* /data/ && cat /data/* > /dev/null && ls -l /data"
Write-Host "volume $vol ready"
