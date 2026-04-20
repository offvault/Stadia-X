@echo off
setlocal enabledelayedexpansion
color 0B
echo =========================================
echo    Stadia X - Native Bridge
echo =========================================
echo.
set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

echo [System Check] Verifying environment...

usbipd --version >nul 2>&1
if %errorlevel% neq 0 (
    echo [Setup] USBIPD not found. Installing...
    winget install usbipd
    echo.
    echo ==========================================================
    echo SETUP REQUIREMENT: USBIPD has been installed.
    echo Please RESTART YOUR COMPUTER, then run this script again.
    echo ==========================================================
    pause
    exit /b
)

if not exist "%USERPROFILE%\wsl_kernel" (
    echo [Setup] Deploying custom WSL kernel...
    copy "%SCRIPT_DIR%\build\wsl_kernel" "%USERPROFILE%\wsl_kernel" >nul
    echo [wsl2] > "%USERPROFILE%\.wslconfig"
    echo kernel=C:\\Users\\%USERNAME%\\wsl_kernel >> "%USERPROFILE%\.wslconfig"
    echo memory=800MB >> "%USERPROFILE%\.wslconfig"
    echo processors=2 >> "%USERPROFILE%\.wslconfig"
    echo swap=800MB >> "%USERPROFILE%\.wslconfig"
)


wsl -d Ubuntu echo ok >nul 2>&1
if %errorlevel% neq 0 (
    echo [Setup] Installing Ubuntu WSL userland...
    wsl --install -d Ubuntu
    echo.
    echo ==========================================================
    echo SETUP REQUIREMENT: Ubuntu installed.
    echo Please RESTART YOUR COMPUTER, then run this script again.
    echo ==========================================================
    pause
    exit /b
)


taskkill /F /IM stadia_receiver.exe >nul 2>&1
wsl --shutdown >nul 2>&1
timeout /t 2 /nobreak >nul

echo [1/4] Starting WSL...
wsl echo "WSL Booted" >nul 2>&1
timeout /t 2 /nobreak >nul

echo.
echo [2/4] Attaching Bluetooth Hardware...

REM ---- Universal Bluetooth Auto-Detection ----
set "BT_BUSID="
echo Auto-detecting Bluetooth adapter...

REM Search usbipd for common Bluetooth device identifiers
for /f "tokens=1" %%a in ('usbipd list ^| findstr /i /c:"bluetooth" /c:"intel wireless" /c:"intel(r) wireless" /c:"realtek" /c:"mediatek" /c:"qualcomm"') do (
    set "BT_BUSID=%%a"
    goto :BT_FOUND
)

:BT_FOUND
if "%BT_BUSID%"=="" (
    echo WARNING: Could not auto-detect Bluetooth adapter by name.
    echo Available devices:
    usbipd list
    echo.
    set /p "BT_BUSID=Enter the BUSID of your Bluetooth adapter (e.g. 1-13): "
)

if "%BT_BUSID%"=="" (
    echo ERROR: No Bluetooth adapter provided. Cannot continue.
    pause
    exit /b 1
)

echo Success: Target Bluetooth adapter found on BUSID %BT_BUSID%

echo %BT_BUSID%> "%SCRIPT_DIR%\bt_busid.txt"

usbipd bind --busid %BT_BUSID% --force >nul 2>&1
usbipd attach --wsl --busid %BT_BUSID%
if %errorlevel% neq 0 (
    echo WARNING: First attach failed. Retrying in 3 seconds...
    timeout /t 3 /nobreak >nul
    usbipd attach --wsl --busid %BT_BUSID%
    if %errorlevel% neq 0 (
        echo ERROR: Could not attach Bluetooth to WSL. Check usbipd and firewall.
        pause
        exit /b 1
    )
)

echo.
echo [3/4] Deploying to Linux...
for /f "delims=" %%i in ('wsl wslpath -u "%SCRIPT_DIR%"') do set WSL_PATH=%%i
wsl -u root mkdir -p /opt/stadia-x
wsl -u root cp "%WSL_PATH%/start.sh" /opt/stadia-x/start.sh
wsl -u root cp "%WSL_PATH%/stadia_bridge" /opt/stadia-x/stadia_bridge
wsl -u root sed -i "s/\r//g" /opt/stadia-x/start.sh
wsl -u root chmod +x /opt/stadia-x/start.sh

echo.
echo [4/4] Starting Services...

start /MIN "Stadia X - Linux Core" wsl -u root bash -c "/opt/stadia-x/start.sh"

timeout /t 8 /nobreak >nul
set WSL_IP=
for /f "tokens=2 delims=:" %%a in ('wsl bash -c "ip addr show eth0 2^>nul ^| grep 'inet ' ^| head -1 ^| cut -d' ' -f6 ^| cut -d'/' -f1"') do set WSL_IP=%%a
if "%WSL_IP%"=="" set WSL_IP=172.25.125.101

echo.
echo =====================================================================
echo   GAME ON!
echo   When you are done, close the "Stadia Receiver" window.
echo   Stop-Stadia will run automatically to restore your Bluetooth.
echo =====================================================================


start /B powershell -WindowStyle Hidden -Command ^
    "Start-Process -FilePath '%SCRIPT_DIR%\stadia_receiver.exe' -ArgumentList '%WSL_IP%' -WorkingDirectory '%SCRIPT_DIR%' -Wait; ^
     Start-Process -FilePath 'cmd.exe' -ArgumentList '/c ""%SCRIPT_DIR%\Stop-Stadia.bat""' -Verb RunAs"

exit