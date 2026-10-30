# Runs one or more built images on a track's full data (Docker volume sd-<track>,
# see get-data.ps1), alternating run by run so drift hits them all equally, and
# prints the median and minimum wall time, the median cgroup peak memory and the
# output hash of each. Limits match the board: 12 CPUs, 8 GB, read-only root.
#
#   docker build --network=none -t mine-b track-b
#   powershell -File tools\cmp.ps1 -Track b -Images mine-b
#   powershell -File tools\cmp.ps1 -Track b -Images old-b,new-b -Runs 9
#
# The timer wraps the process inside the container, so container start-up is
# excluded. Laptop numbers only rank versions; the board's number is the real one.
param(
    [Parameter(Mandatory = $true)][ValidateSet("a", "b", "c", "d")][string]$Track,
    [Parameter(Mandatory = $true)][string[]]$Images,
    [int]$Runs = 5
)

$output = @{ a = "summary.txt"; b = "results.txt"; c = "aggregates.txt"; d = "distances.txt" }[$Track]
$res = @{}
$cmd = @{}
foreach ($img in $Images) {
    $res[$img] = @()
    # Run whatever the image's ENTRYPOINT is, but under our timer.
    $ep = docker image inspect -f "{{json .Config.Entrypoint}}" $img | ConvertFrom-Json
    if (-not $ep) { throw "image $img not found or has no ENTRYPOINT" }
    $cmd[$img] = $ep -join " "
}

foreach ($i in 1..$Runs) {
    foreach ($img in $Images) {
        $script = 's=$(date +%s%N); ' + $cmd[$img] + ' >/dev/null 2>&1; c=$?; e=$(date +%s%N); ' +
                  'echo $(( (e-s)/1000000 )) $(sha256sum /out/' + $output + ' 2>/dev/null | cut -c1-16) ' +
                  '$(( $(cat /sys/fs/cgroup/memory.peak)/1048576 )) $c'
        $o = docker run --rm --cpus=12 --memory=8g --memory-swap=8g --read-only `
            --tmpfs /tmp:rw,size=1g,exec --tmpfs /out -e TRACK=$Track `
            -v "sd-${Track}:/data:ro" --entrypoint sh $img -c $script
        $res[$img] += , ($o -split " ")
    }
}

foreach ($img in $Images) {
    $ms = $res[$img] | ForEach-Object { [int]$_[0] } | Sort-Object
    $mb = $res[$img] | ForEach-Object { [int]$_[2] } | Sort-Object
    $hashes = ($res[$img] | ForEach-Object { $_[1] } | Sort-Object -Unique) -join ","
    $codes = ($res[$img] | ForEach-Object { $_[3] } | Sort-Object -Unique) -join ","
    "{0,-16} median {1,6} ms   min {2,6} ms   peak {3,5} MB   exit {4}   sha256 {5}" -f `
        $img, $ms[[int]($ms.Count / 2)], $ms[0], $mb[[int]($mb.Count / 2)], $codes, $hashes
}
