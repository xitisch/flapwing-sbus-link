# SBUS Controller for a Flapping-Wing Robot

A host GUI sends control-channel values to an ESP32, which drives the robot's
flight controller over a standard **SBUS** signal. The repo contains two
variants of the link:

- **`wired/`** — the original single-board bridge. The PC talks to one ESP32
  over USB serial, and that ESP32 outputs SBUS directly to the robot.
- **`wireless/`** — a two-board ESP-NOW link. The PC talks to an ESP32
  transmitter, which relays the channels wirelessly to an ESP32-C3 SuperMini
  receiver on the robot that outputs the SBUS signal.

Both variants speak the same host protocol, so `gui.py` is functionally
identical in each: it sends `<CH1,CH2,CH3,CH5,CH6,CH8>` over USB serial.

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
│   └── gui.py                    # Tkinter slider GUI (same as wired/gui.py)
├── README.md
└── .gitignore
```

> The two `wireless/.../esp_now_link.h` files are intentional duplicates:
> Arduino sketch folders are self-contained. Keep them identical so the
> `SbusPacket` struct stays binary compatible across both nodes.

## Channels (both variants)

| Value sent | SBUS index | Channel | Function            | Safe default |
|------------|-----------:|---------|---------------------|-------------:|
| 1          | 0          | CH1     | Yaw                 | 1500         |
| 2          | 1          | CH2     | Pitch               | 1500         |
| 3          | 2          | CH3     | Throttle            | 1000 (min)   |
| 4          | 4          | CH5     | Trim 1              | 1500         |
| 5          | 5          | CH6     | Trim 2              | 1500         |
| 6          | 7          | CH8     | Throttle lock / arm | 1000 (locked) |

All values are in microseconds, range `[1000, 2000]`. CH8 is controlled by the
GUI's throttle-lock toggle and starts locked/disarmed.

## Common setup

- Install the **ESP32 Arduino core** (Boards Manager) and the
  **bolderflight/sbus** library (Library Manager). `esp_now` and `WiFi` ship
  with the ESP32 core.
- `pip install pyserial` for the GUI.
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
PC and an ESP32-C3 SuperMini receiver mounted on the robot. The ESP-NOW payload
is unchanged, so the two different ESP32 chips remain wirelessly compatible.

### Receiver wiring

| ESP32-C3 SuperMini | Flight controller |
|--------------------|-------------------|
| GPIO4              | SBUS input        |
| GND                | GND               |

GPIO4 is driven directly as an inverted 3.3 V SBUS signal. Do not use the old
DevKitV1 GPIO17 wiring with the SuperMini; GPIO17 is not exposed for this use on
the C3 board.

### Build & flash (Arduino IDE)

1. Install the Espressif **ESP32 Arduino core** and **Bolder Flight Systems
   SBUS 8.1.4** from Library Manager.
2. Open `wireless/receiver/receiver.ino`. Select **Nologo ESP32C3 Super Mini**
   if your installed core provides it; otherwise select **ESP32C3 Dev Module**.
   Set **USB CDC On Boot** to **Enabled**, then flash the receiver.
3. Open the receiver's Serial Monitor at 115200 baud. It should print
   `Receiver MAC`, `SBUS output: GPIO4`, and `ESP-NOW receiver ready`.
4. The transmitter now uses the broadcast address by default, so it works with
   the new C3 without editing a MAC. To restrict it to one receiver, replace
   `RECEIVER_MAC` in `wireless/transmitter/transmitter.ino` with the address
   printed in step 3.
5. Open `wireless/transmitter/transmitter.ino`, select **DOIT ESP32 DEVKIT V1**,
   and flash the PC-tethered node. Close its Serial Monitor afterward so the GUI
   can claim the port.

If a SuperMini upload does not start, hold **BOOT**, tap **RESET**, begin the
upload, and release **BOOT** when the IDE starts connecting.

### Build & flash (PlatformIO)

Each wireless node is an independent PlatformIO project. The receiver project
uses the generic 4 MB ESP32-C3 target, enables native USB CDC, pins the
toolchain, and installs the SBUS library automatically.

Build, upload, and monitor the ESP32-C3 SuperMini receiver:

```bash
python -m pip install platformio
python -m platformio run --project-dir wireless/receiver
python -m platformio run --project-dir wireless/receiver --target upload --upload-port COMx
python -m platformio device monitor --project-dir wireless/receiver --baud 115200 --port COMx
```

Replace `COMx` with the SuperMini's port. PlatformIO's generic
`esp32-c3-devkitm-1` target is intentional; its official registry does not have
a separate SuperMini board definition.

Build the DevKitV1 transmitter from its own project:

```bash
python -m platformio run --project-dir wireless/transmitter
python -m platformio run --project-dir wireless/transmitter --target upload --upload-port COMx
python -m platformio device monitor --project-dir wireless/transmitter --baud 115200 --port COMx
```

### Run the GUI

```bash
python wireless/gui.py
```

The GUI auto-detects the transmitter's USB-serial port (prompting if several
candidates are found) and exposes a slider per channel. Moving a slider sends a
packet to the transmitter, which forwards it over ESP-NOW. The automated
wireless benchmark ramps CH3 at 250 channel units per second; ordinary manual
slider changes remain immediate. While the benchmark runs, its button becomes
an immediate stop button that locks CH8.

Both nodes explicitly use Wi-Fi channel 1. If you change
`ESPNOW_WIFI_CHANNEL`, update both copies of `esp_now_link.h`.

### Failsafe

The GUI sends the current controls to the transmitter every 100 ms. If those
host packets stop for 500 ms, the transmitter locks CH8 and marks its ESP-NOW
packet as failsafe. The receiver then sets the SBUS `failsafe` flag even though
the ESP-NOW radio link is still alive. After a host timeout, the transmitter
must receive CH8 locked once before it will accept another unlock command.
When the GUI receives this failsafe report, it stops any running benchmark,
synchronizes its CH8 control to locked, and displays instructions for checking
the robot before deliberately unlocking again.

Separately, the transmitter resends its current state over ESP-NOW every ~50
ms. If the receiver hears nothing for `LINK_TIMEOUT_MS` (500 ms, see
`esp_now_link.h`), it sets throttle to minimum, locks CH8, and sets the SBUS
`failsafe` flag until the wireless link recovers.
