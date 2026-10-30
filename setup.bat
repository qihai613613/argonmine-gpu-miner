@echo off
setlocal
cd /d "%~dp0"

tasklist /FI "IMAGENAME eq argonmine-gpu.exe" 2>nul | find /I "argonmine-gpu.exe" >nul
if not errorlevel 1 (
  echo The miner is running. Stop it first with stop.bat.
  pause
  exit /b 1
)

set "OLDADDR="
set "OLDBATCH=1024"
if exist argonmine-gpu.cfg (
  set /p OLDADDR=<argonmine-gpu.cfg
  for /f "usebackq tokens=1,* delims==" %%A in ("argonmine-gpu.cfg") do (
    if /i "%%A"=="batch" set "OLDBATCH=%%B"
  )
)

echo Argonmine GPU miner - settings
echo.
if defined OLDADDR echo Current address: %OLDADDR%
set /p ADDR=ARG address [%OLDADDR%]: 
if "%ADDR%"=="" set "ADDR=%OLDADDR%"
if "%ADDR%"=="" (
  echo No address entered.
  pause
  exit /b 1
)

echo.
echo VRAM = batch x 8 MiB.  768 = 6 GiB, 1024 = 8 GiB, 1536 = 12 GiB.
echo Keep some VRAM free for Windows and the display.
set /p BATCH=Batch [%OLDBATCH%]: 
if "%BATCH%"=="" set "BATCH=%OLDBATCH%"

> argonmine-gpu.cfg echo %ADDR%
>> argonmine-gpu.cfg echo batch=%BATCH%

echo.
echo Saved to argonmine-gpu.cfg:
type argonmine-gpu.cfg
echo.
echo 1 = save and start now
echo 2 = save only
set /p GO=Choose [1]: 
if "%GO%"=="2" exit /b 0
call watchdog.bat
