#!/bin/bash
echo "[Stadia X] Initializing Bluetooth..."
mkdir -p /dev/input/
modprobe uhid joydev hid_generic 2>/dev/null || true
service dbus restart
killall bluetoothd 2>/dev/null || true
bluetoothd &
sleep 2

bluetoothctl power on
bluetoothctl scan on >/dev/null 2>&1 &
sleep 4

echo "[Stadia X] Scanning for Stadia controller..."
STADIA_MAC=$(bluetoothctl devices | grep -i "Stadia" | head -n 1 | awk '{print $2}')

if [ -z "$STADIA_MAC" ]; then
    echo "[Stadia X] No previously paired Stadia found. Waiting for connection..."
    echo "[Stadia X] Hold the Stadia button + Y on your controller for pairing mode."
    # Wait up to 60s for a Stadia device to appear
    for i in $(seq 1 30); do
        sleep 2
        STADIA_MAC=$(bluetoothctl devices | grep -i "Stadia" | head -n 1 | awk '{print $2}')
        if [ -n "$STADIA_MAC" ]; then break; fi
    done
fi

if [ -z "$STADIA_MAC" ]; then
    echo "[ERROR] No Stadia controller found. Exiting."
    read -p "Press Enter to exit..."
    exit 1
fi

echo "[Stadia X] Connecting to $STADIA_MAC..."
bluetoothctl trust "$STADIA_MAC" >/dev/null 2>&1
bluetoothctl connect "$STADIA_MAC" >/dev/null 2>&1 &
sleep 3

WIN_IP=$(ip route | grep default | awk '{print $3}')

echo "[Stadia X] Launching native bridge -> $WIN_IP"
cd /opt/stadia-x
chmod +x stadia_bridge
./stadia_bridge "$WIN_IP"

read -p "Press Enter to exit..."