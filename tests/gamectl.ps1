# Watches and drives an already-running th06nc.exe for testing. It never starts the game.
#
#   gamectl.ps1 wait [-TimeoutSec 600]        block until the game window exists
#   gamectl.ps1 status                        process state
#   gamectl.ps1 shot  -Out file.png [-MaxWidth 1000]   capture the game window
#   gamectl.ps1 key   -Keys Z                 tap a key
#   gamectl.ps1 key   -Keys W,J -HoldMs 800   hold several keys together
#
# Keys are only sent while the game window is in the foreground.
param(
    [Parameter(Mandatory, Position = 0)][ValidateSet('wait', 'status', 'shot', 'key')][string]$Command,
    [string]$Out,
    [string[]]$Keys,
    [int]$HoldMs = 70,
    [int]$TimeoutSec = 600,
    [int]$MaxWidth = 0,
    [string]$ProcessName = 'th06nc'
)
$ErrorActionPreference = 'Stop'

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class GameCtl
{
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)]
    struct KEYBDINPUT { public ushort vk; public ushort scan; public uint flags; public uint time; public IntPtr extra; }
    // The union's largest member (MOUSEINPUT) is 32 bytes on x64.
    [StructLayout(LayoutKind.Explicit, Size = 40)]
    struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }

    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] inputs, int size);

    static GameCtl() { SetProcessDPIAware(); }

    public static void Capture(IntPtr hwnd, string path, int maxWidth)
    {
        RECT r;
        GetWindowRect(hwnd, out r);
        using (var bmp = new Bitmap(Math.Max(1, r.R - r.L), Math.Max(1, r.B - r.T)))
        {
            using (var g = Graphics.FromImage(bmp))
            {
                IntPtr dc = g.GetHdc();
                PrintWindow(hwnd, dc, 2); // PW_RENDERFULLCONTENT: needed for Direct3D windows
                g.ReleaseHdc(dc);
            }
            if (maxWidth <= 0 || bmp.Width <= maxWidth) { bmp.Save(path, ImageFormat.Png); return; }
            using (var small = new Bitmap(bmp, maxWidth, bmp.Height * maxWidth / bmp.Width))
                small.Save(path, ImageFormat.Png);
        }
    }

    public static bool Focus(IntPtr hwnd)
    {
        if (GetForegroundWindow() == hwnd) return true;
        SetForegroundWindow(hwnd);
        System.Threading.Thread.Sleep(150);
        return GetForegroundWindow() == hwnd;
    }

    public static void Key(ushort scan, bool down)
    {
        var i = new INPUT[1];
        i[0].type = 1; // INPUT_KEYBOARD
        i[0].ki.scan = (ushort)(scan & 0xFF);
        i[0].ki.flags = 0x0008u /*SCANCODE*/ | (down ? 0u : 0x0002u /*KEYUP*/) | (scan > 0xFF ? 0x0001u /*EXTENDED*/ : 0u);
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@

# Set-1 scancodes; 0xE0xx = extended key.
$Scan = @{
    ESC = 0x01; ENTER = 0x1C; SPACE = 0x39; LCTRL = 0x1D; LSHIFT = 0x2A
    UP = 0xE048; DOWN = 0xE050; LEFT = 0xE04B; RIGHT = 0xE04D
    Q = 0x10; W = 0x11; E = 0x12; R = 0x13; T = 0x14; Y = 0x15; U = 0x16; I = 0x17; O = 0x18; P = 0x19
    A = 0x1E; S = 0x1F; D = 0x20; F = 0x21; G = 0x22; H = 0x23; J = 0x24; K = 0x25; L = 0x26
    Z = 0x2C; X = 0x2D; C = 0x2E; V = 0x2F; B = 0x30; N = 0x31; M = 0x32
}

function Get-Game {
    Get-Process $ProcessName -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
}

switch ($Command) {
    'wait' {
        $deadline = (Get-Date).AddSeconds($TimeoutSec)
        while (-not ($g = Get-Game)) {
            if ((Get-Date) -gt $deadline) { 'timeout: game not started'; exit 1 }
            Start-Sleep -Milliseconds 500
        }
        "game running: pid $($g.Id), window '$($g.MainWindowTitle)'"
    }
    'status' {
        $all = @(Get-Process $ProcessName -ErrorAction SilentlyContinue)
        if (-not $all) { 'not running'; exit 1 }
        $all | ForEach-Object { "pid $($_.Id) window=$($_.MainWindowHandle) responding=$($_.Responding) title='$($_.MainWindowTitle)'" }
    }
    'shot' {
        $g = Get-Game
        if (-not $g) { 'not running'; exit 1 }
        [GameCtl]::Capture($g.MainWindowHandle, $Out, $MaxWidth)
        "saved $Out"
    }
    'key' {
        $g = Get-Game
        if (-not $g) { 'not running'; exit 1 }
        # `powershell -File` passes "W,J" as one string, so split it here.
        $Keys = $Keys | ForEach-Object { $_ -split '[,+]' } | Where-Object { $_ }
        $codes = foreach ($k in $Keys) {
            if (-not $Scan.ContainsKey($k.ToUpper())) { throw "unknown key '$k'" }
            [uint16]$Scan[$k.ToUpper()]
        }
        if (-not [GameCtl]::Focus($g.MainWindowHandle)) { 'game window is not in the foreground; nothing sent'; exit 1 }
        foreach ($c in $codes) { [GameCtl]::Key($c, $true) }
        Start-Sleep -Milliseconds $HoldMs
        foreach ($c in $codes) { [GameCtl]::Key($c, $false) }
        "sent $($Keys -join '+') for ${HoldMs}ms"
    }
}
