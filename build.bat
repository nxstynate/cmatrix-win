@echo off
REM Build script for CMatrix Windows
REM Run from a Visual Studio Developer Command Prompt, or with MinGW in PATH

where cl >nul 2>&1
if %ERRORLEVEL%==0 (
    echo Building with MSVC...
    cl /O2 /W3 cmatrix_win.c /Fe:cmatrix.exe
    goto done
)

where gcc >nul 2>&1
if %ERRORLEVEL%==0 (
    echo Building with GCC...
    gcc -O2 -Wall -o cmatrix.exe cmatrix_win.c
    goto done
)

where zig >nul 2>&1
if %ERRORLEVEL%==0 (
    echo Building with Zig CC...
    zig cc -O2 -o cmatrix.exe cmatrix_win.c
    goto done
)

echo ERROR: No C compiler found. Install MSVC, MinGW, or Zig.
exit /b 1

:done
if exist cmatrix.exe (
    echo.
    echo Build successful: cmatrix.exe
    echo Run: cmatrix.exe
    echo Try: cmatrix.exe -c -B    (Japanese chars, all bold)
    echo Try: cmatrix.exe -r       (rainbow mode)
) else (
    echo Build failed.
    exit /b 1
)
