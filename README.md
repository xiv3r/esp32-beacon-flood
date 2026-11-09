## What You Need
- ESP32 board (any variant)
- Arduino IDE with ESP32 board support installed
- USB cable

## Setup

1. **Open the sketch** in Arduino IDE (the `.ino` file).

2. **Select your board**: Tools → Board → ESP32 Dev Module (or your specific board).

3. **Select the port**: Tools → Port → (your COM port).

4. **Upload** the sketch (→ button).

5. or download the firmware and flash
   - 

## How to Use

1. **Power the ESP32** (USB or battery). The onboard LED (GPIO 2) stays OFF until you start.

2. **Connect to the Wi-Fi network**:
   - SSID: `ESP32_Beacon_Flood`
   - Password: `12345678`

3. **Open a browser** and go to: `http://192.168.4.1`

4. **Fill the form**:
   - **Wi-Fi name (SSID)**: base name for fake networks (e.g. `FreeWiFi`)
   - **Total Wi-Fi**: how many fake networks to create (1–1000)
   - **Channel**: 1–14 (pick the channel you want to flood)

5. **Tap START BROADCAST**. The page shows:
   - Status → ACTIVE
   - Packet count → live counter
   - ESP temp → chip temperature (turns red above 65 °C)

6. **Tap STOP BROADCAST** to stop.

## Notes
- The LED (GPIO 2) turns ON while broadcasting.
- If the ESP gets hot (>65 °C shown in red), stop and let it cool.
- To change the AP name/password, edit `AP_SSID` and `AP_PASS` at the top of the sketch and re-upload.
- If the portal doesn't open, reconnect to the ESP32 Wi-Fi and try `http://192.168.4.1` again.

## Troubleshooting
| Problem | Fix |
|---|---|
| Can't find `ESP32_Beacon_Flood` | Re-upload; check LED blinks on boot |
| Portal won't load | Make sure you're connected to the ESP32's Wi-Fi, not mobile data |
| Upload fails | Hold BOOT button while uploading; check driver/port |
| Stops responding | Power-cycle the board |

⚠️ **Use only on your own devices/networks.** Flooding Wi-Fi channels can disrupt nearby networks and may be illegal in your area.
