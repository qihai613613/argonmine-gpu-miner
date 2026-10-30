@echo off
cd /d "%~dp0"
echo stop > stop.flag
taskkill /IM argonmine-gpu.exe /F >nul 2>&1
echo Miner stopped. The watchdog will exit on its next loop.
