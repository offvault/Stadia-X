@echo off
color 0C
echo =========================================
echo    Stadia X - Teardown
echo =========================================
echo.
set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

echo [1/3] Terminating processes...
taskkill /F /IM stadia_receiver.exe >nul 2>&1

echo [2/3] Detaching Bluetooth adapter...
set BT_BUSID=

:: Primary: use the saved bus ID from when we started
if exist "%SCRIPT_DIR%\bt_busid.txt" (
    set /p BT_BUSID=<"%SCRIPT_DIR%\bt_busid.txt"
    echo    Using saved Bus ID: %BT_BUSID%
) else (
    :: Fallback 1: device might still be named "bluetooth"
    for /f "tokens=1 delims= " %%a in ('usbipd list ^| findstr /i "bluetooth"') do (
        if "!BT_BUSID!"=="" set BT_BUSID=%%a
    )
    :: Fallback 2: device was renamed to "USBIP Shared Device" after attach
    if "%BT_BUSID%"=="" (
        for /f "tokens=1 delims= " %%a in ('usbipd list ^| findstr /i "USBIP Shared"') do (
            if "!BT_BUSID!"=="" set BT_BUSID=%%a
        )
    )
    :: Fallback 3: Intel Wireless
    if "%BT_BUSID%"=="" (
        for /f "tokens=1 delims= " %%a in ('usbipd list ^| findstr /i "intel wireless"') do (
            if "!BT_BUSID!"=="" set BT_BUSID=%%a
        )
    )
)

if "%BT_BUSID%"=="" (
    echo    No Bluetooth adapter found to detach. It may already be released.
) else (
    echo    Releasing Bus ID: %BT_BUSID%
    usbipd detach --busid %BT_BUSID% >nul 2>&1
    usbipd unbind  --busid %BT_BUSID% >nul 2>&1
    echo    Bluetooth adapter returned to Windows.
    if exist "%SCRIPT_DIR%\bt_busid.txt" del "%SCRIPT_DIR%\bt_busid.txt"
)

echo [3/3] Shutting down WSL...
wsl --shutdown

echo.
echo =========================================
echo   Teardown complete.
echo   Your Bluetooth has been restored.
echo =========================================
timeout /t 5