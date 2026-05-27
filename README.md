# Bluetooth Foot Pedal System

3-button BLE foot pedal with 4 configurable profiles, built on ESP32.

## Hardware

### Components
- ESP32 Dev Module (classic WROOM)
- 3x Momentary foot switches (normally open)
- 1x 4-position rotary switch
- 1x Common-cathode RGB LED
- 3x 220Ω resistors (for RGB LED)

### Wiring

```
ESP32 Pin    Component           Notes
─────────    ─────────           ─────
GPIO 32      Pedal 1 (Left)      Other leg to GND. Internal pull-up used.
GPIO 33      Pedal 2 (Center)    Other leg to GND. Internal pull-up used.
GPIO 25      Pedal 3 (Right)     Other leg to GND. Internal pull-up used.

GPIO 26      Rotary Pos 1        Common to GND. Internal pull-up used.
GPIO 27      Rotary Pos 2        Common to GND. Internal pull-up used.
GPIO 14      Rotary Pos 3        Common to GND. Internal pull-up used.
GPIO 12      Rotary Pos 4        Common to GND. Internal pull-up used.

GPIO 16      RGB LED Red         Via 220Ω resistor to LED anode (R)
GPIO 17      RGB LED Green       Via 220Ω resistor to LED anode (G)
GPIO 18      RGB LED Blue        Via 220Ω resistor to LED anode (B)
             RGB LED Cathode     To GND
```

### Schematic

```
                  ESP32
              ┌───────────┐
    Pedal 1 ──┤ GPIO32    │
    Pedal 2 ──┤ GPIO33    │
    Pedal 3 ──┤ GPIO25    │
              │           │         ┌──── 220Ω ──── R ───┐
    Rot 1 ────┤ GPIO26    │         │                    │
    Rot 2 ────┤ GPIO27  16├─────────┘    ┌── 220Ω ── G ──┤ RGB
    Rot 3 ────┤ GPIO14  17├──────────────┘               │ LED
    Rot 4 ────┤ GPIO12  18├───────────── 220Ω ──── B ────┤
              │           │                              │
              │       GND ├──────────────────────────────┘
              └───────────┘

    Pedals: Switch between GPIO pin and GND (normally open)
    Rotary: Common pin to GND, each position connects one GPIO to GND
```

## LED Behavior

| State                  | LED                              |
|------------------------|----------------------------------|
| Profile 1 active       | Solid Red                        |
| Profile 2 active       | Solid Green                      |
| Profile 3 active       | Solid Blue                       |
| Profile 4 active       | Solid Purple                     |
| BLE not connected      | Flashing Red (up to 5 minutes)   |
| BLE not connected >5m  | LED off                          |

## Default Key Mappings

| Profile | Pedal 1       | Pedal 2    | Pedal 3                |
|---------|---------------|------------|------------------------|
| 1       | Ctrl+Z (Undo) | Ctrl+S (Save) | Ctrl+Shift+Z (Redo) |
| 2       | Page Up       | Space      | Page Down              |
| 3       | Prev Track    | Play/Pause | Next Track             |
| 4       | Left Arrow    | F5         | Right Arrow            |

## Building the Firmware

### Prerequisites
- [PlatformIO CLI](https://platformio.org/install/cli) or [VS Code + PlatformIO extension](https://platformio.org/install/ide?install=vscode)

### Build & Flash
```bash
cd firmware
pio run --target upload
```

### Monitor Serial Output
```bash
cd firmware
pio device monitor
```

## Companion App

### Prerequisites
- Python 3.10+

### Install Dependencies
```bash
cd companion-app
pip install -r requirements.txt
```

### Run
```bash
python pedal_config.py
```

### Build Distributable EXE
```bash
cd companion-app
pyinstaller build.spec
```
The output EXE will be in `companion-app/dist/FootPedalConfigurator.exe`.

## PC Macro Runner (no ESP32 required)

A standalone desktop app that runs the same kind of loops/macros as the foot
pedal system, but entirely on your PC — no ESP32 or Bluetooth hardware needed.
It types the keystrokes itself and triggers loops with a **global hotkey**
(works in any window) or **in-app Play/Stop buttons**.

### Run
```bash
cd companion-app
pip install -r requirements.txt
python macro_runner.py
```

### Usage
1. Click **+ Add Loop** to create a loop, give it a name.
2. Add steps with **+ Add Step**. Each step has an **Action**:
   - **Tap** — press a key/combo once; the time is the *delay* before the next step.
   - **Hold** — press a key/combo and hold it down for the set time, then release.
   - **Wait** — do nothing for the set time (a pause between steps).
3. Choose a **Mode**:
   - **Run once** — play the steps a single time, then stop.
   - **Repeat continuously** — repeat until you stop it.
   - **Repeat _N_ times** — repeat the whole sequence a set number of times.
   - **Repeat for** a number of **Seconds**/**Minutes** — repeat until the time
     is up. The pass in progress always finishes, so a run can slightly exceed
     the limit.
4. Click **Set Hotkey** and press the key combo you want (e.g. `F8`), then press
   that hotkey in any window to start/stop the loop. Or use the **Play/Stop**
   buttons in the app.
5. Loops and hotkeys are saved to `companion-app/macro_settings.json` (on **Save**
   and on exit) and reloaded on the next launch.

> Tip: prefer the global hotkey — keystrokes go to whichever window is focused,
> so clicking **Play** inside the app types into the previously focused window.

### Build Distributable EXE
```bash
cd companion-app
pyinstaller macro_runner.spec
```
The output EXE will be in `companion-app/dist/MacroRunner.exe`.

## Usage

1. Flash the firmware to your ESP32
2. Power on the ESP32 — LED will flash red until a BLE host connects
3. Pair the ESP32 with your PC via Bluetooth settings (device name: "FootPedal")
4. LED shows profile color, foot pedals send configured keystrokes
5. Use the rotary switch to change profiles
6. Run the companion app to customize key mappings (connects via BLE)
