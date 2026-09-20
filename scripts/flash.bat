@echo off
setlocal
cd /d "%~dp0.."
set PYTHONIOENCODING=utf-8

rem ===========================================================
rem  STM32 OTA firmware downloader (XMODEM-CRC, 128-byte blocks)
rem
rem    double-click                 -> COM8 + default region-A image
rem    flash.bat COM3               -> specify serial port
rem    flash.bat COM3 myfw.bin      -> specify port and image
rem
rem  NOTE: this file is intentionally ASCII-only.
rem  cmd.exe parses .bat files using the active code page, and
rem  non-ASCII content (e.g. Chinese) makes it abort mid-file --
rem  the window just flashes and closes.
rem  All Chinese messages come from xmodem_send.py instead.
rem ===========================================================

set PORT=%~1
if "%PORT%"=="" set PORT=COM8

set FWARG=
if not "%~2"=="" set FWARG=--file "%~2"

where python >nul 2>&1
if errorlevel 1 (
    echo.
    echo [ERROR] "python" was not found on PATH.
    echo         Install Python first, then run this again.
    goto :end
)

python "%~dp0xmodem_send.py" --port %PORT% %FWARG%

if errorlevel 1 (
    echo.
    echo [FAILED] Look at the messages above to find the cause.
) else (
    echo.
    echo [DONE] The board should have reset and jumped to region A.
)

:end
echo.
pause
