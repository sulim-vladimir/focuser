# Focuser

Motorised focuser for the 150/750 reflector (rack-and-pinion, 1.25"): Arduino Nano + DRV8825
stepper driver, belt drive to the focus knob. Speaks the **Moonlite** protocol, so it works with the
INDI/Ekos **MoonLite** driver, and remembers its position across power cycles.

## Hardware

| Signal | Nano pin |
|---|---|
| STEP / DIR | A0 / A1 |
| SLP (driver sleep) | D5 |
| M0 / M1 / M2 (microstepping, 1/32) | D11 / D10 / D9 |
| In / out buttons (to GND) | D3 / D2 |

The driver needs its own 12 V supply; USB only powers the Nano.

Mechanics (measured 2026-10-18):

| | |
|---|---|
| Belt | 16T motor : 36T knob (2.25 : 1) |
| Microsteps per knob turn | 14400 |
| Drawtube travel per knob turn | 23.9 mm |
| One microstep | 1.66 µm |
| Microsteps per mm | 601 |

## Flashing

The board is a clone Nano (CH340 USB chip, `1a86:7523`) with the **old bootloader**:

```
arduino-cli lib install AccelStepper
arduino-cli compile --fqbn arduino:avr:nano:cpu=atmega328old -u -p /dev/ttyUSB0 .
```

- Keep only one `.ino` in this folder: arduino-cli compiles every `.ino` in a sketch folder together.
- Disconnect the focuser in Ekos first, it holds the serial port.
- On Ubuntu, if the Nano shows in `lsusb` but no `/dev/ttyUSB0` appears, the braille-display service
  grabbed it: `sudo apt remove brltty`, then replug.

## Behaviour

- **Position** is 0–65535, larger = drawtube out. An empty EEPROM starts at 30000 (~50 mm of room
  each way). The position is saved every time the motor stops, in a wear-levelled EEPROM ring.
- **Buttons** jog while held, slowly for fine manual focus. Jogs stop at 0 and 65535.
- **Sleep**: the driver is switched off 5 s after the last move (no holding torque). `:SK01#` keeps it
  powered permanently (saved in EEPROM), `:SK00#` restores sleep.
- **Speeds** (Ekos "Focus Speed" → microsteps/s):

  | Ekos speed | Moonlite code | µsteps/s | Drawtube |
  |---|---|---|---|
  | 1 | 02 | 800 | 1.3 mm/s |
  | 2 (default) | 04 | 400 | 0.67 mm/s |
  | 3 | 08 | 200 | 0.33 mm/s |
  | 4 | 10 | 100 | 0.17 mm/s |
  | 5 | 20 | 50 | 0.08 mm/s |

  Button jogs use 50 µsteps/s (low, default) or 200 (high), set with `l` / `h`.

## Serial protocol

9600 baud. Opening the port resets the Nano; wait ~2 s before sending.

| Command | Reply | Meaning |
|---|---|---|
| `:GP#` | `XXXX#` | current position (hex) |
| `:GN#` | `XXXX#` | target position |
| `:SPxxxx#` | | set current position (sync) |
| `:SNxxxx#` | | set target position |
| `:FG#` | | go to target |
| `:FQ#` | | stop (decelerates) |
| `:GI#` | `01#` / `00#` | moving / stopped |
| `:GV#` | `10#` | firmware version |
| `:GD#` / `:SDxx#` | `XX#` | speed code (table above) |
| `:GH#` | `00#` | half step (not supported) |
| `:GT#` | `0000#` | temperature (no sensor) |
| `:GC#` | `00#` | temperature coefficient |
| `:GK#` / `:SKxx#` | `01#` / `00#` | keep driver awake (extension) |
| `:SH#` `:SF#` `:SCxx#` `:POxx#` `:C#` `:+#` `:-#` | | accepted, ignored |

Legacy single characters, outside a `:...#` frame: `i` / `o` jog in / out until `s` (stop),
`h` / `l` high / low jog speed.

## Ekos setup

In the Ekos profile choose the **MoonLite** focuser, port `/dev/ttyUSB0` (or the
`/dev/serial/by-id/usb-1a86_USB2.0-Serial-...` link), 9600 baud. After connecting, the INDI Control
Panel → MoonLite → Main Control shows the position.

## Zero point and session start

**Once:** rack the focuser fully in (in button or by hand on the knob, never a long Ekos move into
the hard stop), then INDI Control Panel → MoonLite → Main Control → **Sync** = 0 → Set. The firmware
never goes below 0, so the inner end is now also a soft limit for Ekos and the in button.

Then move out to just short of the outer end, note the position and enter a slightly lower value as
**Max Position** on the same tab, so Ekos (including autofocus) stays inside the travel. Note the
in-focus position of each camera too: next time go there directly and fine-tune with autofocus.

**Each session:** just connect. The position is saved every time the motor stops, so it stays valid
across power-offs.

**Re-zero** (rack fully in, Sync = 0) only if the knob was turned by hand while the motor was asleep,
the motor stalled against an end, or steps might otherwise have been lost.

Autofocus is still needed each session: focus shifts with temperature and with the camera used.

## Autofocus recommendations (150/750, Ekos)

Starting values; refine them from real focus curves.

- **Algorithm:** Linear 1 Pass, measuring star size (HFR). HFR still works on badly defocused,
  donut-shaped stars.
- **Step size: ~50.** The critical focus zone at f/5 is about 67 µm (4.88 · λ · N² with λ = 0.55 µm),
  which is about 40 microsteps. A step slightly larger than that gives a clear V-curve over 9–11
  points. Ekos has a focus-zone calculator in its Focus options to cross-check.
- **AF Overscan: 200–300 steps.** The rack has some slack, so the final approach should always come
  from the same side. Lower this once the real backlash is measured (below).
- **Speed:** 1 or 2. Autofocus steps are small, so speed barely matters.
- **Field:** plenty of medium-bright, unsaturated stars.
- **With the 2x Barlow:** keep the same step size. The focus zone at f/10 is 4x larger, but each
  focuser step also moves the Barlowed focal plane about 4x more, so the tolerance in focuser
  steps stays about 40.

A good result is a V-shaped or parabolic curve with a clear minimum. A flat or noisy curve means
the step size is too small (or seeing is bad); a curve that never turns up means the run started
too far from focus.

### Measuring backlash

1. Move in by 500 steps, so the slack is taken up in the inward direction.
2. Watch a star at high magnification in live view (or a tape mark on the drawtube).
3. Move out in steps of 10 and count the steps until the star (or mark) starts to move.
   That count is the backlash; set AF Overscan to about 2x it.

## Testing after changes

1. In Ekos, set 33000 from 30000: the drawtube must move **out** about 5 mm, then back with 30000.
2. Go 30000 → 35000 → 30000 a few times: a tape mark on the drawtube should line up each time.
3. Move to 31234, disconnect, unplug USB, reconnect: the position should still be 31234.
4. Hold each button ~10 s: about 1 mm of slow movement, stopping on release.
