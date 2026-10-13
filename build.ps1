# Builds dist\th06mp.exe (launcher) and dist\th06mp.dll (the mod) with LLVM-MinGW.
#   powershell -ExecutionPolicy Bypass -File build.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

$clang = Get-Command x86_64-w64-mingw32-clang -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
if (-not $clang) {
    # winget's PATH entry only reaches shells started after the install
    $clang = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW*" -Recurse `
        -Filter x86_64-w64-mingw32-clang.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $clang) { throw 'x86_64-w64-mingw32-clang not found. Install it with: winget install MartinStorsjo.LLVM-MinGW.UCRT' }

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$flags = '-std=c11', '-O2', '-Wall', '-Wextra', '-static'

$modSrc = Get-ChildItem (Join-Path $root 'src\mod') -Filter *.c | ForEach-Object { $_.FullName }
& $clang @flags -shared -o (Join-Path $dist 'th06mp.dll') @modSrc -luser32 -lws2_32
if ($LASTEXITCODE) { throw 'th06mp.dll failed to build' }

& $clang @flags -municode -mwindows -o (Join-Path $dist 'th06mp.exe') (Join-Path $root 'src\launcher\launcher.c') -lntdll
if ($LASTEXITCODE) { throw 'th06mp.exe failed to build' }

# Keep an existing ini: it holds the user's key bindings.
$ini = Join-Path $dist 'th06mp.ini'
if (-not (Test-Path $ini)) { Copy-Item (Join-Path $root 'src\th06mp.ini') $ini }

Get-ChildItem $dist | Select-Object Name, Length, LastWriteTime
