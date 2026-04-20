# 🎮 Stadia X - Native Bridge

Stadia X is a low-latency, native bridge that allows you to use your Google Stadia controller via Bluetooth on Windows, complete with **Rumble Support** and a massive **34-key Macro Shortcut System**.

Because Windows natively struggles with the Stadia controller's Bluetooth implementation, this tool seamlessly passes your Bluetooth adapter into a lightweight, custom-built Linux subsystem (WSL2), connects to the controller instantly, and bridges the inputs back to Windows as a flawless Xbox 360 controller.

## ✨ Features
* **Zero-Configuration:** Automated setup installs everything you need.
* **Full Rumble Support:** Force feedback works flawlessly.
* **Universal Game Compatibility:** Emulates a standard Xbox 360 controller via ViGEmBus.
* **Ultimate Macro Pad:** Hold the Assistant or Capture buttons to turn the rest of your controller into a media remote or keyboard shortcut machine!
* **Auto-Restore:** Automatically returns your Bluetooth adapter to Windows when you close the app.

---

## 🛠️ Requirements
1. **Windows 10 or Windows 11**
2. **Hardware Virtualization Enabled:** Ensure VT-x (Intel) or SVM (AMD) is enabled in your motherboard's BIOS (required for WSL2).
3. **Bluetooth Adapter:** Either a built-in motherboard Wi-Fi/BT card or a USB Bluetooth dongle.

---

## 🚀 Installation & First Run

1. Extract the `Stadia X` folder to a permanent location (e.g., your Desktop or `C:\Program Files\Stadia X`). **Do not run it from inside the ZIP file.**
2. Double-click `Start-Stadia.bat`.
3. **The Setup Phase:** 
   * The script will automatically install `usbipd` and `Ubuntu` for WSL. 
   * **Note:** You will likely be prompted to **Restart your PC** during the first run. Please restart, and then run `Start-Stadia.bat` again.
4. **First Pairing:**
   * Once the script boots Linux, it will look for your controller. 
   * Turn on your Stadia Controller, then hold **Stadia + Y** until the light flashes orange to enter pairing mode.
   * It will connect automatically. Next time you play, you just need to turn the controller on!
5. **Game On!** Leave the black console window open while you play. When you are done, simply close the window and `Stop-Stadia` will automatically run to give your Bluetooth back to Windows.

---

## ⌨️ The Macro Shortcut System
Stadia X unlocks the two middle buttons (**Assistant** and **Capture**) to act like "Shift" keys for your controller.

Open `stadia_buttons.ini` in Notepad to configure your shortcuts. By holding Assistant or Capture, you can press any other button on the controller to trigger Windows keyboard shortcuts, media controls, or volume!

**Examples included by default:**
* `Capture + D-Pad Up/Down` = Volume Up/Volume Down
* `Capture + D-Pad Left/Right` = Next/Previous Track
* `Assistant + L3` = Ctrl+Alt+Delete

---

## ⚠️ Troubleshooting

**1. Windows Defender / SmartScreen blocks the script or `.exe`**
Because Stadia X uses a custom-compiled executable (`stadia_receiver.exe`) to inject controller and keyboard inputs, some Antivirus software may flag it as suspicious. This is a false positive. Click "More Info" -> "Run Anyway", or add the folder to your Antivirus exclusions.

**2. Script crashes with "Virtual Machine Platform is not enabled"**
You need to enable hardware virtualization in your BIOS. Look for `VT-x` (Intel) or `SVM / AMD-V` (AMD) and set it to Enabled.

**3. Script asks for my Bluetooth BUSID manually**
Sometimes Windows names your Bluetooth adapter strangely. If it asks for your BUSID, look at the list printed on the screen, find the item that looks like your Bluetooth adapter (e.g., "Intel Wireless Bluetooth"), and type the number next to it (e.g., `1-14`).

## 🏆 Credits & Acknowledgements
* **[jocxfin]**: Original author of the `stadia-w-rumble-windows` proof-of-concept, which provided the foundational C++ UDP bridge and ViGEm implementation this project was built upon.
* **[Nefarius]**: Creator of the ViGEmBus driver, making Xbox 360 controller emulation possible on Windows.