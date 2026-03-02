#Requires -Version 5.1
<#
.SYNOPSIS
    Build script for CMatrix Windows native port.
.DESCRIPTION
    Auto-detects available C compiler (MSVC cl.exe, GCC, or Zig CC)
    and builds cmatrix.exe. Attempts to import VS environment if cl.exe
    is not already on PATH.
.PARAMETER Compiler
    Force a specific compiler: 'msvc', 'gcc', or 'zig'. Auto-detects if omitted.
.PARAMETER Clean
    Remove build artifacts before building.
.PARAMETER Run
    Run cmatrix.exe after a successful build.
.PARAMETER RunArgs
    Arguments to pass when using -Run (e.g. '-c -B').
.EXAMPLE
    .\build.ps1
    .\build.ps1 -Compiler msvc -Run
    .\build.ps1 -Run -RunArgs '-c -B -r'
    .\build.ps1 -Clean
#>
[CmdletBinding()]
param(
    [ValidateSet('auto', 'msvc', 'gcc', 'zig')]
    [string]$Compiler = 'auto',

    [switch]$Clean,
    [switch]$Run,
    [string]$RunArgs = ''
)

$ErrorActionPreference = 'Stop'
$SrcFile  = 'cmatrix_win.c'
$OutFile  = 'cmatrix.exe'
$ObjFile  = 'cmatrix_win.obj'

# ── Helpers ──────────────────────────────────────────────────────────
function Write-Status  { param([string]$Msg) Write-Host "  [*] $Msg" -ForegroundColor Cyan }
function Write-Success { param([string]$Msg) Write-Host "  [+] $Msg" -ForegroundColor Green }
function Write-Err     { param([string]$Msg) Write-Host "  [-] $Msg" -ForegroundColor Red }

function Test-Command {
    param([string]$Name)
    $null = Get-Command $Name -ErrorAction SilentlyContinue
    return $?
}

function Import-VsEnv {
    <#
    .SYNOPSIS
        Find and import the Visual Studio Developer environment via vsdevcmd.bat or vcvarsall.bat.
    #>
    # Try vswhere first (VS 2017+)
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -property installationPath 2>$null
        if ($installPath) {
            $vcvars = Join-Path $installPath 'VC\Auxiliary\Build\vcvarsall.bat'
            if (Test-Path $vcvars) {
                Write-Status "Importing VS environment from: $installPath"
                # Run vcvarsall and capture the resulting environment
                $arch = if ([Environment]::Is64BitOperatingSystem) { 'x64' } else { 'x86' }
                cmd /c "`"$vcvars`" $arch >nul 2>&1 && set" | ForEach-Object {
                    if ($_ -match '^([^=]+)=(.*)$') {
                        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
                    }
                }
                return $true
            }
        }
    }

    # Fallback: search for vsdevcmd.bat in common locations
    $searchPaths = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\*\*\Common7\Tools\VsDevCmd.bat",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\*\*\Common7\Tools\VsDevCmd.bat"
    )
    foreach ($pattern in $searchPaths) {
        $found = Get-Item $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) {
            Write-Status "Importing VS environment from: $($found.FullName)"
            cmd /c "`"$($found.FullName)`" >nul 2>&1 && set" | ForEach-Object {
                if ($_ -match '^([^=]+)=(.*)$') {
                    [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
                }
            }
            return $true
        }
    }

    return $false
}

# ── Clean ────────────────────────────────────────────────────────────
if ($Clean) {
    Write-Status 'Cleaning build artifacts...'
    Remove-Item $OutFile, $ObjFile -Force -ErrorAction SilentlyContinue
    Write-Success 'Clean.'
    if (-not $Run) { return }
}

# ── Verify source exists ────────────────────────────────────────────
if (-not (Test-Path $SrcFile)) {
    Write-Err "Source file '$SrcFile' not found in $(Get-Location)"
    Write-Err "Run this script from the cmatrix_win directory."
    exit 1
}

# ── Detect / select compiler ────────────────────────────────────────
$selectedCompiler = $null

function Try-MSVC {
    if (Test-Command 'cl') { return $true }
    # cl not on PATH — try importing VS environment
    Write-Status "cl.exe not on PATH, searching for Visual Studio..."
    if (Import-VsEnv) {
        if (Test-Command 'cl') { return $true }
    }
    return $false
}

if ($Compiler -eq 'auto') {
    if (Try-MSVC)           { $selectedCompiler = 'msvc' }
    elseif (Test-Command 'gcc') { $selectedCompiler = 'gcc' }
    elseif (Test-Command 'zig') { $selectedCompiler = 'zig' }
} else {
    switch ($Compiler) {
        'msvc' { if (Try-MSVC)           { $selectedCompiler = 'msvc' } }
        'gcc'  { if (Test-Command 'gcc') { $selectedCompiler = 'gcc'  } }
        'zig'  { if (Test-Command 'zig') { $selectedCompiler = 'zig'  } }
    }
}

if (-not $selectedCompiler) {
    Write-Err "No C compiler found."
    Write-Host ""
    Write-Host "  Options:" -ForegroundColor Yellow
    Write-Host "    1. Open a Visual Studio Developer Command Prompt and re-run"
    Write-Host "    2. Install MinGW-w64: winget install -e --id MingW-w64.MingW-w64"
    Write-Host "    3. Install Zig:       winget install -e --id zig.zig"
    Write-Host "    4. Install MSVC:      winget install -e --id Microsoft.VisualStudio.2022.BuildTools"
    exit 1
}

# ── Build ────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "  CMatrix Windows Build" -ForegroundColor White
Write-Host "  =====================" -ForegroundColor DarkGray

$sw = [System.Diagnostics.Stopwatch]::StartNew()

switch ($selectedCompiler) {
    'msvc' {
        Write-Status "Building with MSVC (cl.exe)..."
        & cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS $SrcFile /Fe:$OutFile 2>&1 | ForEach-Object {
            Write-Host "       $_" -ForegroundColor DarkGray
        }
        # Clean up MSVC artifacts
        Remove-Item $ObjFile -Force -ErrorAction SilentlyContinue
    }
    'gcc' {
        Write-Status "Building with GCC..."
        & gcc -O2 -Wall -Wextra -o $OutFile $SrcFile 2>&1 | ForEach-Object {
            Write-Host "       $_" -ForegroundColor DarkGray
        }
    }
    'zig' {
        Write-Status "Building with Zig CC..."
        & zig cc -O2 -o $OutFile $SrcFile 2>&1 | ForEach-Object {
            Write-Host "       $_" -ForegroundColor DarkGray
        }
    }
}

$sw.Stop()

if (-not (Test-Path $OutFile)) {
    Write-Host ""
    Write-Err "Build FAILED."
    exit 1
}

$size = (Get-Item $OutFile).Length
$sizeStr = if ($size -gt 1MB) { "{0:N1} MB" -f ($size / 1MB) }
           elseif ($size -gt 1KB) { "{0:N0} KB" -f ($size / 1KB) }
           else { "$size bytes" }

Write-Host ""
Write-Success "Build OK  ($($sw.ElapsedMilliseconds)ms, $sizeStr)"
Write-Host ""
Write-Host "       .\$OutFile            # default green" -ForegroundColor DarkGray
Write-Host "       .\$OutFile -c -B      # katakana, all bold" -ForegroundColor DarkGray
Write-Host "       .\$OutFile -r         # rainbow mode" -ForegroundColor DarkGray
Write-Host "       .\$OutFile -C red -s  # red screensaver" -ForegroundColor DarkGray
Write-Host ""

# ── Optional run ─────────────────────────────────────────────────────
if ($Run) {
    Write-Status "Launching cmatrix..."
    if ($RunArgs) {
        $argArray = $RunArgs -split '\s+'
        & ".\$OutFile" @argArray
    } else {
        & ".\$OutFile"
    }
}
