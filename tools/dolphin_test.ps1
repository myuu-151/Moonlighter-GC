# Runs a test build's disc (TTEST=1: MLGC/Source/TranslatedTest.cpp) in Dolphin on a hidden Windows
# desktop, so no window ever shows on the user's screen (Moonlighter-Recomp's tools/oracle_launch.ps1
# does the same for the game: CreateDesktop, then CreateProcess on it), and writes what the program
# printed (its OSReport lines, from Dolphin's log file) to -Out.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\dolphin_test.ps1 [-Iso <disc>] [-Out <file>]
#
# Dolphin runs from a private user folder (build\dolphin-user, made from DolphinWorks' "Accurate"
# profile: one CPU thread, the CPU cache, FPRF, accurate NaNs, no fastmem, the DSP's LLE) with the
# Null video backend, unthrottled (the timebase counts emulated cycles, not the PC's time), its log
# on file with only the OSReport channels. It's stopped when the log has -Done ("MLGC TTEST DONE"),
# or after -TimeoutSec (exit 124). Exits 0 if the marker came.
param(
    [string]$Iso = '',
    [string]$Out = '',
    [string]$Dolphin = 'C:\DolphinWorks\Dolphin',
    [ValidateSet('Accurate', 'Fast')][string]$Profile = 'Accurate',
    [string]$Done = 'MLGC TTEST DONE',
    [int]$TimeoutSec = 3600,
    [string]$DesktopName = 'MLGCDolphinTest'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Iso) { $Iso = Join-Path $repo 'MLGC\Packaged\GameCube\MLGC.iso' }
if (-not $Out) { $Out = Join-Path $repo 'build\dolphin_test.log' }
$exe = Join-Path $Dolphin 'Dolphin.exe'
if (-not (Test-Path $exe)) { throw "no Dolphin.exe in $Dolphin" }
if (-not (Test-Path $Iso)) { throw "no disc: $Iso" }

# The private user folder: the profile's settings, our log settings.
$user = Join-Path $repo 'build\dolphin-user'
$config = Join-Path $user 'Config'
New-Item -ItemType Directory -Force $config | Out-Null
$source = Join-Path $Dolphin $(if ($Profile -eq 'Accurate') { 'User-Accurate\Config' } else { 'User\Config' })
Copy-Item (Join-Path $source '*.ini') $config -Force
@'
[Options]
Verbosity = 4
WriteToFile = True
WriteToConsole = False
WriteToWindow = False
[Logs]
OSREPORT = True
OSREPORT_HLE = True
'@ | Set-Content -Encoding ascii (Join-Path $config 'Logger.ini')
$log = Join-Path $user 'Logs\dolphin.log'
Remove-Item -Force -ErrorAction SilentlyContinue $log

$arguments = "-u `"$user`" -b -v Null -C Dolphin.Core.EmulationSpeed=0 -C Dolphin.Interface.ConfirmStop=False -e `"$Iso`""

if (-not ('MlgcHiddenDesktop' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class MlgcHiddenDesktop {
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateDesktopW(string name, IntPtr device, IntPtr devmode, uint flags, uint access, IntPtr sa);
    [DllImport("user32.dll", SetLastError = true)]
    static extern bool CloseDesktop(IntPtr desktop);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct STARTUPINFO {
        public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
                                      IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    const uint GENERIC_ALL = 0x10000000;
    static IntPtr desktop = IntPtr.Zero;

    public static int Start(string desktopName, string exe, string args, string cwd, uint priorityFlag) {
        desktop = CreateDesktopW(desktopName, IntPtr.Zero, IntPtr.Zero, 0, GENERIC_ALL, IntPtr.Zero);
        if (desktop == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateDesktop");
        var si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        si.lpDesktop = "WinSta0\\" + desktopName;
        PROCESS_INFORMATION pi;
        var cmd = new StringBuilder("\"" + exe + "\" " + args);
        if (!CreateProcessW(exe, cmd, IntPtr.Zero, IntPtr.Zero, false, priorityFlag, IntPtr.Zero, cwd, ref si, out pi))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateProcess");
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return pi.dwProcessId;
    }

    public static void Close() {
        if (desktop != IntPtr.Zero) { CloseDesktop(desktop); desktop = IntPtr.Zero; }
    }
}
'@
}

# Below normal priority: the user may be using the machine.
$id = [MlgcHiddenDesktop]::Start($DesktopName, $exe, $arguments, $Dolphin, [uint32]0x4000)
$p = Get-Process -Id $id
$null = $p.Handle
$code = 124
$deadline = (Get-Date).AddSeconds($TimeoutSec)
try {
    while ((Get-Date) -lt $deadline) {
        if ($p.HasExited) { $code = 2; break }
        if ((Test-Path $log) -and (Select-String -Path $log -SimpleMatch $Done -Quiet)) { $code = 0; break }
        Start-Sleep -Seconds 5
    }
    if ($code -eq 124) { Write-Warning "dolphin_test: no '$Done' after $TimeoutSec s" }
    if ($code -eq 2) { Write-Warning 'dolphin_test: Dolphin quit before the test was done' }
} finally {
    if (-not $p.HasExited) {
        try { $p.Kill(); $p.WaitForExit(10000) | Out-Null } catch {}
    }
    [MlgcHiddenDesktop]::Close()
}

# What the program printed: the OSReport lines, without Dolphin's prefix.
New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
if (Test-Path $log) {
    Get-Content $log | ForEach-Object {
        if ($_ -match '\[OSREPORT(?:_HLE)?\]: ?(.*)$') { $Matches[1] }
    } | Set-Content -Encoding utf8 $Out
} else {
    Set-Content -Encoding utf8 $Out ''
}
Write-Output "dolphin_test: $Out (exit $code)"
exit $code
