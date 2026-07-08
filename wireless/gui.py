"""
gui.py

Tkinter GUI for sending SBUS channel values to the transmitter ESP32 over USB
serial. The transmitter relays them over ESP-NOW to the receiver ESP32, which
drives the robot's SBUS input. Each slider maps to one SBUS channel; moving any
slider immediately transmits a packet to the transmitter firmware.

Serial packet format:  <CH1,CH2,CH3,CH5,CH6,CH8>\n
All values in range [1000, 2000].

Usage:
    python gui.py
    The script auto-detects the transmitter's serial port.  If multiple
    candidates are found, a small picker dialog appears first.
"""

import tkinter as tk
from tkinter import ttk
import serial
import serial.tools.list_ports
import time

BAUD_RATE = 115200

# Wireless test steps: (status text, duration in ms, channel values to update)
WIRELESS_TEST_STEPS = (
    ("Throttle armed; throttle 1000", 5000, {"ch8": 2000, "ch3": 1000}),
    ("Throttle 1200", 3000, {"ch3": 1200}),
    ("Throttle 1600", 3000, {"ch3": 1600}),
    ("Throttle 2000", 3000, {"ch3": 2000}),
    ("Neutral break before yaw", 3000, {"ch3": 1000, "ch1": 1500, "ch2": 1500}),
    ("Yaw 1000", 3000, {"ch1": 1000}),
    ("Yaw 1500", 3000, {"ch1": 1500}),
    ("Yaw 2000", 3000, {"ch1": 2000}),
    ("Neutral break before pitch", 3000, {"ch3": 1000, "ch1": 1500, "ch2": 1500}),
    ("Pitch 1000", 3000, {"ch2": 1000}),
    ("Pitch 1500", 3000, {"ch2": 1500}),
    ("Pitch 2000", 3000, {"ch2": 2000}),
)

# Safe/neutral values restored after the wireless test completes
WIRELESS_TEST_FINAL_VALUES = {"ch1": 1500, "ch2": 1500, "ch3": 1000, "ch8": 1000}

suppress_slider_send = False

# USB-serial chip substrings found in ESP32/Arduino port descriptions
_KNOWN_CHIPS = ('cp210', 'ch340', 'ch341', 'ftdi', 'uart', 'arduino', 'esp')


def select_port():
    """Return a serial port device string, auto-detecting or prompting the user.

    Filters available ports by known USB-serial chip names.  If exactly one
    candidate is found it is returned immediately.  If multiple are found a
    small picker dialog is shown.  Returns None if no ports are available.
    """
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        return None

    matches = [p for p in ports
               if any(kw in (p.description or '').lower() for kw in _KNOWN_CHIPS)]
    candidates = matches or ports   # fall back to all ports if no keyword hit

    if len(candidates) == 1:
        return candidates[0].device

    # Multiple candidates — show a blocking picker before the main window
    picker = tk.Tk()
    picker.title("Select Serial Port")
    picker.geometry("440x110")
    picker.resizable(False, False)

    tk.Label(picker, text="Multiple ports found. Select the ESP32 port:").pack(pady=8)

    labels  = [f"{p.device}  —  {p.description}" for p in candidates]
    devices = [p.device for p in candidates]
    var = tk.StringVar(value=labels[0])
    ttk.Combobox(picker, textvariable=var, values=labels,
                 state='readonly', width=55).pack(padx=20)

    chosen = [devices[0]]

    def confirm():
        chosen[0] = devices[labels.index(var.get())]
        picker.destroy()

    tk.Button(picker, text="Connect", command=confirm).pack(pady=8)
    picker.mainloop()
    return chosen[0]


SERIAL_PORT = select_port()

try:
    if SERIAL_PORT is None:
        raise OSError("No serial ports found")
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    print(f"Connected to {SERIAL_PORT}")
    time.sleep(2)   # ESP32 resets on USB connect; wait for it to finish booting
except Exception as e:
    print(f"Could not open serial port: {e}")
    ser = None


def send_channels(*_):
    """Read the five sliders plus the CH8 toggle and send one packet to the ESP32."""
    if not (ser and ser.is_open):
        return

    try:
        ch1 = int(slider_ch1.get())
        ch2 = int(slider_ch2.get())
        ch3 = int(slider_ch3.get())
        ch5 = int(slider_ch5.get())
        ch6 = int(slider_ch6.get())
        ch8 = int(ch8_var.get())
    except NameError:
        # Called before the widgets exist (tkinter fires command during widget creation)
        return

    cmd = f"<{ch1},{ch2},{ch3},{ch5},{ch6},{ch8}>\n"
    try:
        ser.write(cmd.encode('utf-8'))
        label_status.config(text=f"Sent: {cmd.strip()}", fg="green")
        return True
    except Exception:
        label_status.config(text="Send failed", fg="red")
        return False


def create_slider(label_text, default_val, min_val=1000, max_val=2000,
                  label_font=("Arial", 11), value_font=("Arial", 11),
                  label_width=14, pady=6):
    """Add a labeled horizontal slider row to root and return the Scale widget.

    The value box is an editable Entry: dragging the slider updates the box,
    and typing a value then pressing Enter moves the slider to that value.
    Fonts and padding are parameterized so the primary flight controls can be
    rendered larger than the secondary trim controls.
    """
    frame = tk.Frame(root)
    frame.pack(fill='x', padx=20, pady=pady)

    tk.Label(frame, text=label_text, width=label_width, anchor='w',
             font=label_font).pack(side='left')

    val_var = tk.StringVar(value=str(default_val))
    val_entry = tk.Entry(frame, textvariable=val_var, width=6, justify='center',
                         font=value_font)
    val_entry.pack(side='right')

    slider = ttk.Scale(frame, from_=min_val, to=max_val, orient='horizontal')
    slider.set(default_val)
    slider.pack(side='left', fill='x', expand=True, padx=8)

    # Slider -> box: reflect the slider value in the box as it moves, then send
    def on_slider_change(v):
        val_var.set(str(int(float(v))))
        if not suppress_slider_send:
            send_channels()
    slider.config(command=on_slider_change)

    # Box -> slider: typing a value and pressing Enter moves the slider
    def on_entry(_event=None):
        try:
            val = int(float(val_var.get()))
        except ValueError:
            val = int(slider.get())            # revert to current on invalid input
        val = max(min_val, min(max_val, val))  # clamp to the channel range
        val_var.set(str(val))
        slider.set(val)                        # fires the slider command -> sends
    val_entry.bind('<Return>', on_entry)

    slider.value_var = val_var
    return slider


root = tk.Tk()
root.title("Flapping-Wing SBUS Controller")
root.geometry("520x585")

tk.Label(root, text="ESP32 Flapping-Wing Controller",
         font=("Arial", 14, "bold")).pack(pady=(12, 6))

# --- CH8 throttle lock: master enable, shown as a big button at the very top ---
# 1000 = locked/disarmed, 2000 = unlocked/armed. Default locked for safety.
ch8_var = tk.IntVar(value=1000)

def refresh_ch8_button():
    """Update the CH8 button's text/colour to match its current state."""
    if ch8_var.get() == 2000:
        ch8_btn.config(text="THROTTLE LOCK (CH8):   UNLOCKED - ARMED",
                       bg="#2e7d32", activebackground="#2e7d32", fg="white")
    else:
        ch8_btn.config(text="THROTTLE LOCK (CH8):   LOCKED - DISARMED",
                       bg="#c62828", activebackground="#c62828", fg="white")

def toggle_ch8():
    ch8_var.set(1000 if ch8_var.get() == 2000 else 2000)
    refresh_ch8_button()
    send_channels()

def set_slider_value(slider, value):
    """Set a slider and its editable value box to the same integer value."""
    slider.value_var.set(str(value))
    slider.set(value)

def apply_channel_values(values):
    """Apply channel values and send one complete packet to the ESP32."""
    global suppress_slider_send

    suppress_slider_send = True
    try:
        if "ch1" in values:
            set_slider_value(slider_ch1, values["ch1"])
        if "ch2" in values:
            set_slider_value(slider_ch2, values["ch2"])
        if "ch3" in values:
            set_slider_value(slider_ch3, values["ch3"])
        if "ch5" in values:
            set_slider_value(slider_ch5, values["ch5"])
        if "ch6" in values:
            set_slider_value(slider_ch6, values["ch6"])
        if "ch8" in values:
            ch8_var.set(values["ch8"])
            refresh_ch8_button()
    finally:
        suppress_slider_send = False

    return send_channels()

def run_wireless_test(step_index=0):
    """Run the butterfly wireless test without blocking the Tkinter loop."""
    if step_index == 0:
        wireless_test_btn.config(state='disabled', text="Wireless Test Running...")

    if step_index >= len(WIRELESS_TEST_STEPS):
        apply_channel_values(WIRELESS_TEST_FINAL_VALUES)
        label_status.config(text="Wireless test complete: controls returned to safe neutral",
                            fg="green")
        wireless_test_btn.config(state='normal', text="Run Wireless Communications Test")
        return

    step_text, duration_ms, values = WIRELESS_TEST_STEPS[step_index]
    sent = apply_channel_values(values)
    if sent:
        label_status.config(text=f"Wireless test: {step_text}", fg="blue")
    else:
        label_status.config(text=f"Wireless test queued locally: {step_text}", fg="orange")

    root.after(duration_ms, lambda: run_wireless_test(step_index + 1))

ch8_btn = tk.Button(root, command=toggle_ch8, font=("Arial", 13, "bold"),
                    height=2, relief="raised", bd=3)
ch8_btn.pack(fill='x', padx=20, pady=(0, 14))
refresh_ch8_button()

# --- Butterfly wireless communications test: timed channel sweep ---
wireless_test_btn = tk.Button(root, text="Run Wireless Communications Test",
                              command=run_wireless_test,
                              font=("Arial", 11, "bold"), height=2,
                              relief="raised", bd=2)
wireless_test_btn.pack(fill='x', padx=20, pady=(0, 14))

# --- Primary flight controls: large and prominent ---
tk.Label(root, text="Flight Controls", font=("Arial", 10, "bold"),
         fg="#333333", anchor='w').pack(fill='x', padx=20)

_big_label = ("Arial", 12, "bold")
_big_value = ("Arial", 12, "bold")
slider_ch1 = create_slider("Yaw  (CH1)", 1500,
                           label_font=_big_label, value_font=_big_value, pady=9)
slider_ch2 = create_slider("Pitch  (CH2)", 1500,
                           label_font=_big_label, value_font=_big_value, pady=9)
slider_ch3 = create_slider("Throttle  (CH3)", 1000,  # start at minimum
                           label_font=_big_label, value_font=_big_value, pady=9)

# --- Trim controls: secondary, smaller, at the bottom ---
ttk.Separator(root, orient='horizontal').pack(fill='x', padx=20, pady=(14, 4))
tk.Label(root, text="Trim (servo center)", font=("Arial", 9),
         fg="gray", anchor='w').pack(fill='x', padx=20)

_small_label = ("Arial", 9)
_small_value = ("Arial", 9)
slider_ch5 = create_slider("Trim 1 (CH5)", 1500, label_font=_small_label,
                           value_font=_small_value, label_width=12, pady=1)
slider_ch6 = create_slider("Trim 2 (CH6)", 1500, label_font=_small_label,
                           value_font=_small_value, label_width=12, pady=1)

port_text = SERIAL_PORT if SERIAL_PORT else "No port"
label_status = tk.Label(root, text=f"Connected: {port_text}", bd=1, relief='sunken', anchor='w')
label_status.pack(side='bottom', fill='x', padx=10, pady=10)

root.mainloop()

# Release the port after the window closes
if ser and ser.is_open:
    ser.close()
    print("Serial port closed")
