# SBUS Controller for a Flapping-Wing Robot

A host GUI sends control-channel values to an ESP32, which drives the robot's
flight controller over a standard **SBUS** signal. The repo contains two
variants of the link:

- **`wired/`** — the original single-board bridge. The PC talks to one ESP32
  over USB serial, and that ESP32 outputs SBUS directly to the robot.
- **`wireless/`** — a two-board ESP-NOW link. The PC talks to an ESP32
  transmitter, which relays the channels wirelessly to an ESP32-C3 SuperMini
  receiver on the robot that outputs the SBUS signal.

Both variants use the same host packet format:
`<CH1,CH2,CH3,CH5,CH6,CH8>`. The wireless GUI additionally provides the
automated ramped benchmark, GUI heartbeat, host-failsafe warning, and immediate
benchmark stop control.

```
            wired/                                 wireless/

 PC (gui.py)                            PC (gui.py)
    │ USB serial <CH1,..>                  │ USB serial <CH1,..>
    ▼                                      ▼
 ESP32 ──SBUS──▶ robot          ESP32 transmitter ──ESP-NOW──▶ ESP32-C3 receiver
                                                                   │ SBUS (GPIO4)
                                                                   ▼
                                                                 robot
```

## Repository layout

```
.
├── wired/
│   ├── SBUS.ino                  # single ESP32: USB serial -> SBUS output
│   └── gui.py                    # Tkinter slider GUI
├── wireless/
│   ├── transmitter/
│   │   ├── transmitter.ino       # USB serial -> ESP-NOW   (PC-tethered node)
│   │   ├── esp_now_link.h        # shared ESP-NOW payload definition
│   │   └── platformio.ini        # DevKitV1 transmitter build
│   ├── receiver/
│   │   ├── receiver.ino          # ESP-NOW -> SBUS output  (robot node)
│   │   ├── esp_now_link.h        # identical copy of the payload definition
│   │   └── platformio.ini        # ESP32-C3 SuperMini receiver build
│   └── gui.py                    # wireless control and benchmark GUI
├── README.md
└── .gitignore
```

> The two `wireless/.../esp_now_link.h` files are intentional duplicates:
> Arduino sketch folders are self-contained. Keep them identical so the
> `SbusPacket` struct stays binary compatible across both nodes.

## Channels (both variants)

| Packet field | SBUS index | Channel | Function            | Safe default |
|------------|-----------:|---------|---------------------|-------------:|
| 1          | 0          | CH1     | Yaw                 | 1500         |
| 2          | 1          | CH2     | Pitch               | 1500         |
| 3          | 2          | CH3     | Throttle            | 1000 (min)   |
| 4          | 4          | CH5     | Trim 1              | 1500         |
| 5          | 5          | CH6     | Trim 2              | 1500         |
| 6          | 7          | CH8     | Throttle lock / arm | 1000 (locked) |

Host channel values use the conventional RC range `[1000, 2000]`, with `1500`
as center. They are numeric control values, not physical PWM pulses; the
SBUS-output firmware maps them to SBUS counts. CH8 uses `1000` for
locked/disarmed and `2000` for unlocked/armed, and starts locked.

## Common setup

- For Arduino IDE, install the **ESP32 Arduino core** (Boards Manager) and the
  **bolderflight/sbus** library (Library Manager). `esp_now` and `WiFi` ship
  with the ESP32 core. PlatformIO installs the pinned dependencies from each
  project's `platformio.ini`.
- Install Python 3 and run `python -m pip install pyserial` for the GUI.
- SBUS is inverted, 100kbaud / 8E2. The wired firmware outputs it on
  **GPIO17**; the wireless ESP32-C3 receiver outputs it on **GPIO4**. Always
  connect the MCU and flight-controller grounds.

---

## Wired variant (`wired/`)

The original setup: one ESP32 between the PC and the robot.

1. Flash `wired/SBUS.ino` to the ESP32.
2. Wire the ESP32's GPIO17 to the robot's SBUS input.
3. Run the GUI:
   ```bash
   python wired/gui.py
   ```

The GUI auto-detects the ESP32's USB-serial port and exposes a slider per
channel; moving a slider sends a packet that the ESP32 converts straight to SBUS.

---

## Wireless variant (`wireless/`)

Two boards linked over ESP-NOW: an ESP32 DevKitV1 transmitter connected to the
PC and an ESP32-C3 SuperMini receiver mounted on the robot. Their shared packed
payload contains 16 channel values plus a host-failsafe byte. Both copies of
`esp_now_link.h` must remain identical, and both boards must be reflashed after
any payload-format change.

### Receiver wiring

| ESP32-C3 SuperMini | Flight controller |
|--------------------|-------------------|
| GPIO4              | SBUS input        |
| GND                | GND               |


### Build & flash (Arduino IDE)

1. Install the Espressif **ESP32 Arduino core** and **Bolder Flight Systems
   SBUS 8.1.4** from Library Manager.
2. Open `wireless/receiver/receiver.ino`, select **ESP32C3 Dev Module**, enable
   **USB CDC On Boot**, and flash the receiver.
3. Open the receiver's Serial Monitor at 115200 baud. It should print
   `Receiver MAC`, `SBUS output: GPIO4`, and `ESP-NOW receiver ready`.
4. The transmitter now uses the broadcast address by default, so it works with
   the new C3 without editing a MAC. To restrict it to one receiver, replace
   `RECEIVER_MAC` in `wireless/transmitter/transmitter.ino` with the address
   printed in step 3.
5. Open `wireless/transmitter/transmitter.ino`, select **ESP32 Dev Module**, and
   flash the PC-tethered node. Close its Serial Monitor afterward so the GUI can claim the
   port.

If a SuperMini upload does not start, hold **BOOT**, tap **RESET**, begin the
upload, and release **BOOT** when the IDE starts connecting.

### Build & flash (PlatformIO)

Each wireless node is an independent PlatformIO project. The receiver uses the
generic `esp32-c3-devkitm-1` target with native USB CDC; the transmitter uses
`esp32doit-devkit-v1`.

In VS Code, use **File -> Open Folder** and open only the node you want to
flash:

- `wireless/receiver` for the ESP32-C3 SuperMini receiver.
- `wireless/transmitter` for the ESP32 DevKitV1 transmitter.

PowerShell upload commands from the repository root:

```powershell
# Receiver (currently COM11)
cd wireless\receiver
pio run --target upload --upload-port COM11

# Transmitter (currently COM10)
cd ..\transmitter
pio run --target upload --upload-port COM10
```

PlatformIO can auto-detect the upload port when only one board is connected, so
`--upload-port COMx` may be omitted when only one board is connected. With both
connected, use `pio device list` and specify the port; Windows may change COM
numbers after reconnecting a board.

Only one program can open a COM port at a time. Close the transmitter Serial
Monitor before starting the GUI.

### Run the GUI

```bash
python wireless/gui.py
```

The GUI auto-detects the transmitter port; if prompted, select the DevKitV1
CP210x port (currently COM10). Manual control changes are sent immediately. The
automated benchmark ramps CH3 at 250 units per second; its stop button cancels
the test and locks CH8 immediately.

Both nodes explicitly use Wi-Fi channel 1. If you change
`ESPNOW_WIFI_CHANNEL`, update both copies of `esp_now_link.h`.

### Failsafe

- If GUI packets stop for 500 ms, the transmitter preserves CH3, locks CH8,
  and sets the SBUS failsafe flag. If the GUI remains connected to the
  transmitter, it displays a warning and requires the locked state before the
  user can deliberately unlock again.
- If the ESP-NOW link is lost for 500 ms, the receiver sets CH3 to minimum,
  locks CH8, and sets the SBUS failsafe flag.

The ESP-NOW link is one-way, so receiver-side link loss is visible through the
receiver monitor or flight-controller status rather than the GUI.
