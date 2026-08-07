@echo off
chcp 65001 > nul
echo ========================================
echo   TPMS Emulator Flash Tool v2
echo ========================================
echo.

python flash.py %*

echo.
pause
