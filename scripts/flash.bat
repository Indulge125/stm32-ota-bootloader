@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0.."
set PYTHONIOENCODING=utf-8

rem ============================================================
rem  STM32 OTA 固件下载 —— 双击即用
rem
rem  用法：
rem    直接双击                     用 COM8 + 默认 A 区固件
rem    flash.bat COM3               指定串口
rem    flash.bat COM3 路径\xx.bin   指定串口和固件
rem
rem  前提：已装 Python 和 pyserial，且没有别的程序占用串口
rem ============================================================

set PORT=%~1
if "%PORT%"=="" set PORT=COM8

set FW=%~2
if "%FW%"=="" set FW=1.1-(A区)串口测试程序\Objects\Project.bin

echo ============================================================
echo   STM32 OTA 固件下载（Xmodem-CRC，128 字节包）
echo ============================================================
echo   串口 : %PORT%
echo   固件 : %FW%
echo.
echo   请先关掉占用 %PORT% 的其它程序（串口助手等），否则会打不开串口。
echo   脚本启动后会提示你按板子的复位键，按一下即可。
echo ============================================================
echo.

python "%~dp0xmodem_send.py" --port %PORT% --file "%FW%"
set RC=%errorlevel%

echo.
if not "%RC%"=="0" (
    echo [失败] 脚本返回错误码 %RC%，请看上面的输出定位原因。
) else (
    echo [完成] 板子应已自动复位并跳转到 A 区程序。
)
echo.
pause
