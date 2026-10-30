@echo off
setlocal
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 (
  echo.
  echo Python 3 was not found on PATH.
  echo Install Python 3, or double-click dashboard.html directly
  echo ^(it needs a browser that can reach explorer.argonmine.com^).
  echo.
  pause
  exit /b 1
)

echo Starting Argonmine dashboard...
echo The browser will open automatically.
echo Press Ctrl+C in this window to stop the dashboard.
echo.
python dashboard.py --port 8787
pause
