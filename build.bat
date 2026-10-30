@echo off
setlocal
cd /d "%~dp0"

rem Locate Visual Studio C++ build tools.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if not defined VSDIR (
  echo [ERROR] Visual Studio C++ build tools not found.
  echo         Install "Desktop development with C++" from Visual Studio Installer.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

where nvcc >nul 2>nul
if errorlevel 1 (
  echo [ERROR] nvcc not found. Install the CUDA Toolkit and put it on PATH.
  exit /b 1
)

rem Auto-detect the first NVIDIA GPU's compute capability.
rem RTX 30 = sm_86, RTX 40 = sm_89, RTX 50 = sm_120 (needs CUDA 12.8+).
set "ARCH=sm_86"
for /f "usebackq skip=1 tokens=1,2 delims=." %%a in (`nvidia-smi -i 0 --query-gpu=compute_cap --format=csv 2^>nul`) do set "ARCH=sm_%%a%%b"

echo Building with %ARCH% ...
pushd res
rc /nologo /fo app.res app.rc
if errorlevel 1 (
  popd
  echo [ERROR] resource compile failed.
  exit /b 1
)
popd
nvcc -O3 -std=c++17 -arch=%ARCH% -I src ^
  src\main.cpp src\gpu_miner.cu src\argon2_cpu.cpp src\bech32.cpp ^
  src\http.cpp src\work.cpp res\app.res -o argonmine-gpu.exe
if errorlevel 1 (
  echo [ERROR] build failed.
  exit /b 1
)
del /q *.obj *.lib *.exp res\app.res 2>nul
echo.
echo Built argonmine-gpu.exe
echo Try: argonmine-gpu.exe --selftest
