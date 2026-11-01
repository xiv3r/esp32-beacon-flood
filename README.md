## Overview

Firmware for ESP32 that continuously broadcasts a rotating list of up to **1024 unique WiFi SSIDs** as 802.11 beacon frames, while hosting a small web UI for configuration. Uses `esp_wifi_80211_tx` to inject raw beacon frames with arbitrary SSIDs.

## Hardware requirements

- Any ESP32 (original, S2, S3, C3 — needs `esp_wifi_80211_tx` support)
- BOOT button (GPIO 0) used for factory reset hold

## Quick start

1. Flash the sketch.
2. Connect to AP: SSID `ESP32-Flood`, password printed on serial (format `flood-XXXXXX`).
3. Open `http://192.168.4.1/`.
4. Set SSID prefix, count (1–1024), base (10 or 36), pad width → **Save & Regenerate**.
5. Devices scanning nearby will see the generated SSIDs appear.

## How it works

```
loop() → pollBeacon() → for burst times:
             broadcastOne(ssidList[cursor], currentChannel)
             cursor = (cursor + 1) % ssidCount
```

- **Generator**: builds SSIDs from `prefix + zero-padded index` (decimal or base-36) at boot and on save.
- **Rotation**: round-robin cursor cycles through the list. Every SSID is emitted, but not simultaneously.
- **Burst rate**: `burst` frames per `BEACON_INTERVAL_MS` (default 16 frames / 100 ms = 160 frames/sec).
- **Channel**: fixed to AP channel, or hops 1–13 via `esp_wifi_set_channel` when enabled.
- **Persistence**: full config (34 KB blob) saved to NVS with CRC32, debounced 750 ms.

## Web UI

| Path | Method | Purpose |
|---|---|---|
| `/` | GET | Config form + live status |
| `/save` | POST | Apply settings, regenerate SSIDs |
| `/factory` | POST | Wipe NVS and reboot |
| `/api/status` | GET | JSON: ssids, cursor, chan, ok, err, skip, burst, heap |
| `/api/cfg` | GET | JSON: current config |

## Configuration fields

| Field | Range | Default | Meaning |
|---|---|---|---|
| `apssid` | ≤32 chars | `ESP32-Flood` | AP SSID for management access |
| `appass` | blank or ≥8 | auto `flood-XXXXXX` | AP password (blank = open) |
| `apchan` | 1–13 | 6 | AP channel |
| `prefix` | ≤23 chars | `FreeWiFi_` | Prefix for generated SSIDs |
| `count` | 1–1024 | 512 | Number of SSIDs to generate |
| `base` | 10 or 36 | 36 | Numeric suffix base |
| `pad` | 0–6 | 0 | Zero-pad width for suffix |
| `en` | bool | on | Enable/disable flood |
| `hop` | bool | off | Channel hopping |
| `burst` | 1–64 | 16 | Frames per 100 ms tick |

## Performance tuning

| Goal | Adjust |
|---|---|
| Faster full cycle | ↑ `burst`, ↓ interval (hard-coded 100 ms) |
| Fewer TX errors | ↓ `burst` to 8–12, ↑ interval |
| More SSIDs | ↑ `count` up to 1024 |
| Shorter SSIDs (faster TX) | Small `prefix`, small `pad` |

If `err` in `/api/status` grows quickly, the radio queue is saturated — reduce `burst`.

## Factory reset

- **Via web**: `/factory` button on index page.
- **Via button**: hold BOOT (GPIO 0) for 5 seconds → reboot, NVS wiped.

## Serial output

```
Boot #N
AP SSID: ESP32-Flood
AP PASS: flood-a1b2c3
Beacon flood: 512 SSIDs, burst=16, hop=0
```

## Limitations

- **Not simultaneous**: 1000+ SSIDs are rotated, not broadcast at once — physically impossible on any WiFi radio.
- **TX errors** under high burst: `esp_wifi_80211_tx` returns `ESP_ERR_NO_MEM` when the queue is full.
- **Channel hopping** briefly moves the AP radio, which may drop the management connection.
- **Region**: beacon flooding is illegal in many jurisdictions — use on your own test equipment only.
- **Memory**: config blob is ~34 KB; leaving <50 KB heap free may destabilize the web server.

## Notes

- NVS namespace: `beacon` (config), `sys` (boot count).
- Config is validated on load; out-of-range fields are clamped.
- Beacon frames are open (no RSN IE); clients will see "Open" security. 
