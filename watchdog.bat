@echo off
setlocal
cd /d "%~dp0"

if not exist argonmine-gpu.exe (
  echo argonmine-gpu.exe not found.
  pause
  exit /b 1
)

if not exist argonmine-gpu.cfg (
  echo First run: enter your ARG address.
  set /p ADDR=ARG address ^(arg1...^): 
  if "%ADDR%"=="" exit /b 1
  > argonmine-gpu.cfg echo %ADDR%
)

set /p ADDR=<argonmine-gpu.cfg
if "%ADDR%"=="" (
  echo argonmine-gpu.cfg is empty.
  pause
  exit /b 1
)

set "BATCH=1024"
for /f "usebackq tokens=1,* delims==" %%A in ("argonmine-gpu.cfg") do (
  if /i "%%A"=="batch" set "BATCH=%%B"
)

:loop
if exist stop.flag (
  del stop.flag
  echo Watchdog stopped.
  exit /b 0
)
echo.
echo [%date% %time%] starting miner
argonmine-gpu.exe --address %ADDR% --batch %BATCH%
echo.
echo [%date% %time%] miner exited with code %errorlevel%, restarting in 5 seconds...
timeout /t 5 /nobreak >nul
goto loop
