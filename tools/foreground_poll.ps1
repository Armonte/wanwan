# foreground_poll.ps1 -- the FOCUS PROOF for FM2K_TEST_BACKGROUND.
#
# Two probes, sampled together at a fixed rate for a fixed duration:
#
#   A. FOREGROUND    GetForegroundWindow -> (pid, process, title). A gate run
#      must never appear here. NOTE this probe alone is NOT discriminating:
#      Windows' foreground lock already refuses activation to a process spawned
#      by a non-foreground parent, so a flag-OFF control ALSO reads zero
#      offenders. It is necessary, not sufficient -- which is exactly why probe
#      B exists.
#
#   B. WINDOW STATE  every top-level window owned by a launcher/game process,
#      with IsWindowVisible + IsIconic. THIS is the discriminating measurement:
#      background mode's claim is that those windows exist but are minimized
#      (iconic) and never on top of the owner's work, and the flag-OFF control
#      shows the same windows visible and NOT iconic. Reported as
#      "window-samples: visible_not_iconic / iconic / hidden".
#
#   C. RENDERER FALLBACK (a HARD FAIL, not a statistic). cnc-ddraw renames the
#      game window to "-WARNING- Using slow software rendering, please update
#      your graphics card driver (...)" when its preferred renderer (D3D9/GL)
#      fails to initialize and it degrades to software blitting. That is the
#      exact failure a window hidden or minimized too early can cause, and it
#      distorts frame pacing -- so a run in which ANY sampled game-window title
#      contains "-WARNING-" is NOT a green, however good probes A and B look.
#      Probe C piggybacks on probe B's titles: same samples, no extra cost.
#      (String verified against the literal in 2DFMD.dll.)
#
# Usage (from WSL):
#   powershell.exe -NoProfile -ExecutionPolicy Bypass \
#       -File C:/dev/wanwan/tools/foreground_poll.ps1 \
#       -Seconds 400 -Hz 4 -OutFile C:/path/to/poll.csv
#
# Writes <OutFile> (foreground samples) and <OutFile>.windows.csv (probe B).
# The poller is its own console process and creates no window of its own, so it
# never perturbs what it measures.
param(
    [int]    $Seconds = 300,
    [double] $Hz      = 4.0,
    [string] $OutFile = "foreground_poll.csv"
)

# Process names a gate stage can spawn. Kept in sync with run_all_tests.sh's
# kill_games list -- a name missing here is a blind spot, not a pass.
$OURS = '^(FM2K_RollbackLauncher|WonderfulWorld_ver_0946|WonderfulRvl|vanpri|pkmncc|_hrun|CPW|ＣＰＷ|ShadowArts|kensei2023|DragonPuppy|URORFGRelease102|REQUIEM FINAL|闘闘)'

$sig = @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class FgWin {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public static string Title(IntPtr h) {
        StringBuilder sb = new StringBuilder(512);
        GetWindowTextW(h, sb, 512);
        return sb.ToString();
    }
    // Returns "pid|visible|iconic|title" for every top-level window.
    public static List<string> AllWindows() {
        List<string> outp = new List<string>();
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            uint pid = 0;
            GetWindowThreadProcessId(h, out pid);
            outp.Add(pid.ToString() + "|" + (IsWindowVisible(h) ? "1" : "0")
                     + "|" + (IsIconic(h) ? "1" : "0") + "|" + Title(h));
            return true;
        }, IntPtr.Zero);
        return outp;
    }
}
"@
Add-Type -TypeDefinition $sig -Language CSharp

$interval = [int](1000.0 / $Hz)
$deadline = (Get-Date).AddSeconds($Seconds)
$fgRows  = New-Object System.Collections.Generic.List[string]
$winRows = New-Object System.Collections.Generic.List[string]
$fgRows.Add("iso_time,pid,process,title")
$winRows.Add("iso_time,pid,process,visible,iconic,title")

# Process-name cache: Get-Process per sample would cost more than the interval.
# Entries for dead pids are re-resolved once and then pinned to "<gone>".
$nameCache = @{}
function Resolve-Name([uint32]$p) {
    if ($nameCache.ContainsKey($p)) { return $nameCache[$p] }
    $n = "<gone>"
    try { $n = (Get-Process -Id $p -ErrorAction Stop).ProcessName } catch { }
    if ($n -ne "<gone>") { $nameCache[$p] = $n }
    return $n
}

$nVis = 0; $nIcon = 0; $nHidden = 0
$lastFlush = Get-Date
$swWarnRows = New-Object System.Collections.Generic.List[string]
while ((Get-Date) -lt $deadline) {
    $now = Get-Date -Format "o"

    # --- probe A: foreground -------------------------------------------------
    $h = [FgWin]::GetForegroundWindow()
    $fgpid = [uint32]0
    [void][FgWin]::GetWindowThreadProcessId($h, [ref]$fgpid)
    $fgname = Resolve-Name $fgpid
    $fgRows.Add(("{0},{1},{2},{3}" -f $now, $fgpid, $fgname, ([FgWin]::Title($h) -replace ',', ';')))

    # --- probe B: our windows' state ----------------------------------------
    foreach ($w in [FgWin]::AllWindows()) {
        $parts = $w.Split('|')
        $wpid  = [uint32]$parts[0]
        $wname = Resolve-Name $wpid
        if ($wname -notmatch $OURS) { continue }
        $vis = $parts[1]; $ico = $parts[2]
        if     ($vis -eq "0") { $nHidden++ }
        elseif ($ico -eq "1") { $nIcon++ }
        else                  { $nVis++ }
        $wtitle = $parts[3] -replace ',', ';'
        # probe C: cnc-ddraw's software-rendering fallback marker.
        if ($wtitle -like '*-WARNING-*') {
            $swWarnRows.Add(("{0},{1},{2},{3}" -f $now, $wpid, $wname, $wtitle))
        }
        $winRows.Add(("{0},{1},{2},{3},{4},{5}" -f $now, $wpid, $wname, $vis, $ico, $wtitle))
    }

    # Periodic flush. INSTRUMENT DEFECT FIXED 2026-08-17: this script used to
    # write ONLY at exit, so a 70-minute poll over a 50-minute gate held all its
    # evidence hostage to the timer -- killing it early lost everything, and the
    # data could not be read while the run it was measuring was still going.
    # Flushing every ~15s costs nothing measurable and makes the CSV live.
    if (((Get-Date) - $lastFlush).TotalSeconds -ge 15) {
        $fgRows  | Set-Content -Encoding UTF8 -Path $OutFile
        $winRows | Set-Content -Encoding UTF8 -Path ($OutFile + ".windows.csv")
        $lastFlush = Get-Date
    }

    Start-Sleep -Milliseconds $interval
}

# Final flush (the periodic flush inside the loop already wrote most of this).
$fgRows  | Set-Content -Encoding UTF8 -Path $OutFile
$winRows | Set-Content -Encoding UTF8 -Path ($OutFile + ".windows.csv")

$data  = $fgRows | Select-Object -Skip 1
$total = $data.Count
$offenders = $data | Where-Object { ($_ -split ',')[2] -match $OURS }

Write-Output "[fgpoll] fg-samples=$total fg-offenders=$($offenders.Count) out=$OutFile"
$data | ForEach-Object { ($_ -split ',')[2] } | Group-Object | Sort-Object Count -Descending |
    ForEach-Object { Write-Output ("[fgpoll]   {0,6} x {1}" -f $_.Count, $_.Name) }
if ($offenders.Count -gt 0) {
    Write-Output "[fgpoll] FIRST OFFENDING FOREGROUND SAMPLES:"
    $offenders | Select-Object -First 10 | ForEach-Object { Write-Output "[fgpoll]   $_" }
}
$winTotal = $nVis + $nIcon + $nHidden
Write-Output "[fgpoll] window-samples=$winTotal visible_not_iconic=$nVis iconic=$nIcon hidden=$nHidden"
if ($winTotal -eq 0) {
    Write-Output "[fgpoll] WARNING: probe B saw ZERO launcher/game windows -- either the run never started or the name list is stale. Do NOT read this as a pass."
}
# probe C verdict -- a hard fail, printed last so it is the final line read.
if ($swWarnRows.Count -gt 0) {
    $swPids = ($swWarnRows | ForEach-Object { ($_ -split ',')[1] } | Sort-Object -Unique) -join ' '
    Write-Output "[fgpoll] SWRENDER=FAIL samples=$($swWarnRows.Count) pids=$swPids -- cnc-ddraw fell back to SOFTWARE rendering. This arm is NOT green."
    $swWarnRows | Select-Object -First 5 | ForEach-Object { Write-Output "[fgpoll]   $_" }
} else {
    Write-Output "[fgpoll] SWRENDER=OK -- no game window ever carried cnc-ddraw's '-WARNING-' software-rendering title"
}
