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
              │           │         ┌──── 220Ω ──── R ──┐
    Rot 1 ────┤ GPIO26    │         │                    │
    Rot 2 ────┤ GPIO27  16├─────────┘    ┌── 220Ω ── G ─┤ RGB
    Rot 3 ────┤ GPIO14  17├──────────────┘               │ LED
    Rot 4 ────┤ GPIO12  18├───────────── 220Ω ──── B ──┤
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

| Profile | Pedal 1       | Pedal 2    | Pedal 3          |
|---------|---------------|------------|------------------|
| 1       | Ctrl+Z (Undo) | Ctrl+S (Save) | Ctrl+Shift+Z (Redo) |
| 2       | Page Up       | Space      | Page Down        |
| 3       | Prev Track    | Play/Pause | Next Track       |
| 4       | Left Arrow    | F5         | Right Arrow      |

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

## Usage

1. Flash the firmware to your ESP32
2. Power on the ESP32 — LED will flash red until a BLE host connects
3. Pair the ESP32 with your PC via Bluetooth settings (device name: "FootPedal")
4. LED shows profile color, foot pedals send configured keystrokes
5. Use the rotary switch to change profiles
6. Run the companion app to customize key mappings (connects via BLE)
