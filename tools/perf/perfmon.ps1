# perfmon.ps1: sample the capture programs' CPU, memory and disk I/O once a second for the capture-program comparison
# (tools/perf/README.md). Watches every process whose name matches -Match and appends to -Out; stop with Ctrl+C.
# Uses only what Windows has built in, so it runs on a bare capture machine.
#   powershell -NoProfile -ExecutionPolicy Bypass -File perfmon.ps1 -Out C:\perf\perfmon.csv
# Columns: utc, machine, pid, name, cpu_pct (of the whole machine), cpu_cores, rss_mb (working set), private_mb,
# read_mb / write_mb (since the process started), sys_cpu_pct (whole machine).
param(
    [Parameter(Mandatory = $true)] [string]$Out,
    [double]$Interval = 1.0,
    [double]$Seconds = 0,   # stop after this long (0: until Ctrl+C)
    [string]$Match = '^(slopcap|SharpCap|FireCapture|java|javaw)$',
    [string]$Machine = $env:COMPUTERNAME
)
$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class PerfMonNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct IoCounters { public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes; }
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetProcessIoCounters(IntPtr process, out IoCounters counters);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetSystemTimes(out long idle, out long kernel, out long user);
}
'@

$cores = [Environment]::ProcessorCount
$inv = [Globalization.CultureInfo]::InvariantCulture
$newFile = -not (Test-Path $Out)
$writer = New-Object IO.StreamWriter($Out, $true, (New-Object Text.UTF8Encoding($false)))
if ($newFile) { $writer.WriteLine('utc,machine,pid,name,cpu_pct,cpu_cores,rss_mb,private_mb,read_mb,write_mb,sys_cpu_pct') }

$prevCpu = @{}   # pid -> processor time ticks at the last sample
$clock = [Diagnostics.Stopwatch]::StartNew()
$lastT = 0.0
$idle0 = 0L; $kern0 = 0L; $user0 = 0L
[void][PerfMonNative]::GetSystemTimes([ref]$idle0, [ref]$kern0, [ref]$user0)
Write-Host "perfmon: $cores logical CPUs, sampling every $Interval s into $Out (Ctrl+C stops)"
try {
    while ($Seconds -le 0 -or $clock.Elapsed.TotalSeconds -lt $Seconds) {
        Start-Sleep -Milliseconds ([int]($Interval * 1000))
        $t = $clock.Elapsed.TotalSeconds
        $dt = $t - $lastT
        $lastT = $t
        $utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ', $inv)

        $idle = 0L; $kern = 0L; $user = 0L   # kernel time includes idle time
        [void][PerfMonNative]::GetSystemTimes([ref]$idle, [ref]$kern, [ref]$user)
        $total = ($kern - $kern0) + ($user - $user0)
        $sysPct = if ($total -gt 0) { 100.0 * ($total - ($idle - $idle0)) / $total } else { 0.0 }
        $idle0 = $idle; $kern0 = $kern; $user0 = $user

        $seen = @{}
        foreach ($p in [Diagnostics.Process]::GetProcesses()) {
            if ($p.ProcessName -notmatch $Match) { continue }
            try {
                $cpu = $p.TotalProcessorTime.Ticks
                $io = New-Object PerfMonNative+IoCounters
                [void][PerfMonNative]::GetProcessIoCounters($p.Handle, [ref]$io)
                $rss = $p.WorkingSet64
                $priv = $p.PrivateMemorySize64
            } catch { continue }   # exited, or not ours to read
            $seen[$p.Id] = $true
            if ($prevCpu.ContainsKey($p.Id) -and $dt -gt 0) {
                $usedCores = ($cpu - $prevCpu[$p.Id]) / 1e7 / $dt
                $writer.WriteLine([string]::Format($inv, '{0},{1},{2},{3},{4:F1},{5:F3},{6:F1},{7:F1},{8:F1},{9:F1},{10:F1}',
                    $utc, $Machine, $p.Id, $p.ProcessName, 100.0 * $usedCores / $cores, $usedCores, $rss / 1MB, $priv / 1MB,
                    $io.ReadBytes / 1MB, $io.WriteBytes / 1MB, $sysPct))
            }
            $prevCpu[$p.Id] = $cpu
        }
        foreach ($id in @($prevCpu.Keys)) { if (-not $seen.ContainsKey($id)) { $prevCpu.Remove($id) } }
        $writer.Flush()
    }
} finally {
    $writer.Close()
}
