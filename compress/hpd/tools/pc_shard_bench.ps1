# pc_shard_bench.ps1 — run on OdinsPC (Windows, PowerShell) from the Cypha repo root:
#   powershell -ExecutionPolicy Bypass -File compress\hpd\tools\pc_shard_bench.ps1 -Path <file> [-Threads N]
# Reports the machine (CPU cores, NVIDIA GPU if any), builds hpd, then compresses
# <file> as 1, few and many shards and checks every round trip. Writes
# compress\hpd\tools\pc_results.txt — paste it back into the Claude session.
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [int]$Threads = [Environment]::ProcessorCount
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot   # compress\hpd  (not tested: no PowerShell in the build VM)
$out  = Join-Path $PSScriptRoot "pc_results.txt"
"hpd shard benchmark  $(Get-Date -Format s)" | Out-File $out
try {
    "CPU: $((Get-CimInstance Win32_Processor).Name)  logical cores: $([Environment]::ProcessorCount)" | Out-File $out -Append
    "RAM: {0:N1} GiB" -f ((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB) | Out-File $out -Append
} catch { "CPU: logical cores $([Environment]::ProcessorCount) (no CIM)" | Out-File $out -Append }
if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) {
    nvidia-smi --query-gpu=name,memory.total,compute_cap,driver_version --format=csv | Out-File $out -Append
} else { "GPU: nvidia-smi not found" | Out-File $out -Append }
if (Get-Command nvcc -ErrorAction SilentlyContinue) { (nvcc --version | Select-Object -Last 2) | Out-File $out -Append }
else { "CUDA toolkit: nvcc not found" | Out-File $out -Append }

# Build: MSVC if cl.exe is on PATH (Developer PowerShell), else MinGW g++.
$exe = Join-Path $root (Join-Path "build" "hpd.exe")
New-Item -ItemType Directory -Force (Join-Path $root "build") | Out-Null
Push-Location $root
if (Get-Command cl -ErrorAction SilentlyContinue) {
    cl /nologo /O2 /std:c++20 /arch:AVX2 /EHsc /I include src\hpd.cpp /Fe:$exe | Out-Null
} elseif (Get-Command g++ -ErrorAction SilentlyContinue) {
    g++ -O3 -march=native -std=c++20 -static -I include src/hpd.cpp -o $exe
} else { Pop-Location; throw "need cl.exe (VS Developer PowerShell) or g++ (MinGW-w64) on PATH" }
Pop-Location
"built $exe" | Out-File $out -Append

$size = (Get-Item $Path).Length
"input $Path  $size bytes  threads $Threads" | Out-File $out -Append
$tmp = [System.IO.Path]::GetTempPath()
$tmpA = Join-Path $tmp "hpd_shard.hpd"; $tmpB = Join-Path $tmp "hpd_shard.out"
foreach ($kib in @(0, 16384, 4096, 1024, 256)) {
    $cargs = @("c", "-t", "$Threads"); if ($kib -gt 0) { $cargs += @("-B", "$kib") }
    $t = Measure-Command { & $exe @cargs $Path $tmpA 2>$null }
    $d = Measure-Command { & $exe d -t $Threads $tmpA $tmpB 2>$null }
    $ok = (Get-FileHash $Path).Hash -eq (Get-FileHash $tmpB).Hash
    $c = (Get-Item $tmpA).Length
    $label = if ($kib -eq 0) { "auto" } else { "$kib KiB" }
    $shards = if ($kib -eq 0) { "auto" } else { [math]::Ceiling($size / ($kib * 1024)) }
    "{0,-10} shards {1,6}  bytes {2,12}  bpc {3:N3}  compress {4,7:N1} s ({5,8:N0} KiB/s)  decompress {6,7:N1} s  roundtrip {7}" -f `
        $label, $shards, $c, (8.0 * $c / $size), $t.TotalSeconds, ($size / 1024 / $t.TotalSeconds), $d.TotalSeconds, $ok |
        Out-File $out -Append
}
Remove-Item $tmpA, $tmpB -ErrorAction SilentlyContinue
Get-Content $out
