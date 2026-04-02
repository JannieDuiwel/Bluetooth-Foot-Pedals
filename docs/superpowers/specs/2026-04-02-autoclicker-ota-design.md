# Autoclicker + OTA Update Design
Date: 2026-04-02

## 1. Autoclicker Pedal Type

### Overview
A new pedal type (type=3) that sends repeated mouse clicks at a configurable frequency while active.

### Config Fields (per pedal, when type=3)
| Field | Type | Range | Default | Description |
|-------|------|-------|---------|-------------|
| `click_hz` | uint8 | 1–100 | 5 | Click frequency in Hz |
| `click_button` | uint8 | 0/1/2 | 0 | Mouse button: 0=left, 1=right, 2=middle |
| `click_mode` | uint8 | 0/1 | 0 | 0=hold (click while held), 1=toggle (press to start/stop) |

### ButtonConfig Struct Changes (`main.cpp`)
Add three new fields to `ButtonConfig`:
```cpp
uint8_t click_hz;      // 1-100 Hz
uint8_t click_button;  // 0=left, 1=right, 2=middle
uint8_t click_mode;    // 0=hold, 1=toggle
```
Existing fields (`type`, `key_count`, `keys`, `loop_index`) are unchanged.

### Firmware Behaviour
- A `BleMouse` instance is initialized in `setup()` alongside the existing `BleKeyboard`.
- Click timing is tracked via `millis()`. Per-pedal state: `last_click_ms`, `autoclicker_active` (bool, for toggle mode only).
- **Hold mode (click_mode=0):** While pedal is pressed, fire a click every `1000/click_hz` ms. Stop on release.
- **Toggle mode (click_mode=1):** On each press-down event, toggle `autoclicker_active`. First press → true (start clicking). Second press → false (stop clicking).
- Clicking = `mouse.click(button)` where button maps 0→`MOUSE_LEFT`, 1→`MOUSE_RIGHT`, 2→`MOUSE_MIDDLE`.

### NVS Storage
`ButtonConfig` is stored as a raw byte array in NVS (key `"profiles"`, 12 bytes × 12 buttons). Adding 3 bytes per button expands to 15 bytes × 12. The NVS key changes to `"profiles2"` to avoid reading stale data from old firmware — on first boot with new firmware, profiles reset to defaults.

### BLE Protocol (no change)
Autoclicker fields are carried in the existing `set`/`get` profile commands within the `buttons` JSON array. Firmware ignores fields it doesn't recognize; old firmware will silently ignore the new fields.

### platformio.ini Addition
```ini
lib_deps =
    t-vk/ESP32 BLE Keyboard
    t-vk/ESP32 BLE Mouse
    bblanchon/ArduinoJson
```

---

## 2. OTA Updates

### Overview
The ESP32 connects to WiFi, checks the latest GitHub Release for a newer version tag, and if found, downloads and flashes `firmware.bin` via the Arduino `Update` library. WiFi credentials are provisioned via BLE from the companion app and stored in NVS.

### GitHub Side
- **GitHub Actions** workflow triggers on push to `main`:
  1. Builds firmware with PlatformIO
  2. Creates a GitHub Release tagged `vX.Y.Z` (tag derived from `VERSION` defined in `main.cpp` via a build flag `-DVERSION`)
  3. Attaches `.pio/build/esp32dev/firmware.bin` as a release asset named `firmware.bin`
- Version string is a single source of truth: `#define VERSION "X.Y.Z"` in `main.cpp`. The GitHub Actions workflow reads this value to set the release tag.

### Firmware — WiFi & OTA
**NVS keys:** `wifi_ssid`, `wifi_pass` (strings).

**Boot sequence addition:**
1. Load WiFi credentials from NVS.
2. If credentials exist, attempt WiFi connection (timeout: 10s).
3. If connected, call `checkAndApplyOTA()`.
4. Disconnect WiFi, resume normal BLE operation.

**`checkAndApplyOTA()` logic:**
1. HTTP GET `https://api.github.com/repos/JannieDuiwel/Bluetooth-Foot-Pedals/releases/latest`
2. Parse JSON → extract `tag_name` (e.g. `v1.2.3`) and find the asset where `name == "firmware.bin"` → get its `browser_download_url`
3. Compare tag to compiled `VERSION`. If same or older, return early.
4. HTTP GET the `browser_download_url` → stream into `Update.h`
5. On success: send BLE notify `{"ota":"done"}` → `ESP.restart()`
6. On failure: send BLE notify `{"ota":"error","msg":"..."}` → continue normal boot

**New BLE commands:**
| Command | Direction | Payload | Description |
|---------|-----------|---------|-------------|
| `set_wifi` | app→device | `{"cmd":"set_wifi","ssid":"...","pass":"..."}` | Store WiFi credentials in NVS |
| `ota_check` | app→device | `{"cmd":"ota_check"}` | Manually trigger OTA check |
| `ota_status` | device→app | `{"ota":"checking"\|"up_to_date"\|"updating"\|"done"\|"error","msg":"..."}` | Progress notifications |

**Libraries needed:**
- `WiFi.h` (built-in ESP32)
- `HTTPClient.h` (built-in ESP32)
- `Update.h` (built-in ESP32)
- `ArduinoJson` (already in project)

### Companion App — New "Device" Tab
New tab added to the existing `QTabWidget`:

**WiFi section:**
- SSID field (QLineEdit)
- Password field (QLineEdit, echo mode = password)
- "Save to Device" button → sends `set_wifi` BLE command

**OTA section:**
- "Check for Update" button → sends `ota_check` BLE command
- Status label → updated via `ota_status` BLE notifications
- Progress bar (shown during download, hidden otherwise)

The app listens for `ota_status` notifications on the existing response characteristic and routes them to the Device tab UI.

---

## 3. Out of Scope
- Cursor locking (dropped — ESP32 cannot intercept physical mouse input)
- Rollback / firmware version history
- OTA without WiFi (BLE-based OTA is too slow for practical use)
- Delta/incremental firmware updates

---

## 4. Implementation Order
1. `platformio.ini` — add BLE Mouse library
2. `main.cpp` — ButtonConfig struct, autoclicker loop logic
3. `main.cpp` — WiFi + OTA functions and BLE commands
4. GitHub Actions workflow
5. `pedal_config.py` — Autoclicker UI in PedalWidget
6. `pedal_config.py` — Device tab (WiFi + OTA)
7. `ble_comm.py` — `set_wifi`, `ota_check` helper methods
8. Commit → flash on COM9
