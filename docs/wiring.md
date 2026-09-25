# Wiring guide

> ⚠️ **Golden rule — one shared ground.**
> The following must all connect to **one common GND rail**:
> - Arduino GND
> - 12 V supply GND
> - 5 V supply GND
> - every MOSFET source
> - the relay GND
> - ESP32-CAM GND

## LCD I2C (16×2)

| LCD | Arduino |
|---|---|
| GND | GND |
| VCC | 5V |
| SDA | A4 |
| SCL | A5 |

## DHT22 — temperature and humidity

| DHT22 | Arduino | Note |
|---|---|---|
| VCC (pin 1) | 5V | |
| DATA (pin 2) | D2 | **10 kΩ pull-up between DATA and VCC is required** |
| NC (pin 3) | — | not connected |
| GND (pin 4) | GND | |

## Soil-moisture probe

| Probe | Arduino |
|---|---|
| VCC | 5V |
| GND | GND |
| AO | A0 |
| DO | not used |

Calibrate it in the firmware (`SOIL_RAW_DRY`, `SOIL_RAW_WET`). The raw reading **drops** as the soil gets wetter.

## LDR — light level (voltage divider)

```
5V ── LDR ──┬── A1
            │
          10 kΩ
            │
           GND
```
More light gives a lower LDR resistance, so A1 reads a **higher** value.

## Keypad 4×4

| Keypad | Arduino | Keypad | Arduino |
|---|---|---|---|
| R1 | D3 | C1 | D7 |
| R2 | D4 | C2 | D8 |
| R3 | D5 | C3 | D9 |
| R4 | D6 | C4 | D10 |

## Fans (12 V) — logic-level MOSFET on D11

```
+12V ─────────────► fan + (all 6 fans in parallel)
fan – (joined) ───► MOSFET DRAIN
MOSFET SOURCE ────► GND rail
D11 ── 220 Ω ─────► MOSFET GATE
GATE ── 10 kΩ ────► GND          (pull-down: fans stay OFF while the UNO boots)
1N4007 flyback: anode → DRAIN, cathode → +12V
```

Use a **logic-level** MOSFET (e.g. IRLZ44N), because 5 V on the gate must turn it fully on.

## Water pump — relay on D12

| Relay module | Connects to |
|---|---|
| VCC | 5V |
| GND | GND |
| IN | D12 |

```
+12V ──► COM
NO   ──► pump +
pump – ──► GND
1N4007 flyback across the pump terminals
```

If your relay board switches ON when IN is **LOW**, set `RELAY_ACTIVE_LOW = true` in the firmware.

## RGB LED strip (12 V, common +) — 3 MOSFETs

| Channel | Strip pin | Arduino gate pin |
|---|---|---|
| Common + | + | — (to +12V) |
| Red | R | D13 |
| Green | G | A2 |
| Blue | B | A3 |

Wire each MOSFET the same way:
- Drain → the strip's colour pin.
- Source → GND.
- Gate → 220 Ω → the Arduino pin.
- Gate → 10 kΩ → GND.

## UART — Arduino ↔ ESP32-CAM (level shifting)

The Arduino's TX is 5 V logic, but the ESP32's RX accepts **3.3 V at most**. Without a divider the ESP32 gets damaged.

```
Arduino TX (D1) ── 1 kΩ ──┬──► ESP32 RX (U0R / GPIO3)
                          │
                         2 kΩ
                          │
                         GND

Vout = 5 V × 2k / (1k + 2k) = 3.33 V  ✓

ESP32 TX (U0T / GPIO1) ──► Arduino RX (D0)   (3.3 V is read as HIGH by the UNO)
ESP32 GND              ──► common GND rail
```

⚠️ D0 is also the UNO's USB upload line. **Unplug the ESP32 TX wire while uploading** to the UNO.

## Decoupling capacitors

| Capacitor | Where |
|---|---|
| 1000 µF | 12 V rail → GND, next to the fans |
| 470 µF | 5 V rail → GND, next to the ESP32-CAM (prevents Wi-Fi brown-outs) |
| 100 µF | 5 V rail → GND, next to the Arduino |

## Safe first power-on

1. Disconnect the 12 V supply and power the logic from 5 V only.
2. Check that the Arduino's power LED is on.
3. Upload the UNO sketch over USB, with the ESP32 TX wire removed from D0.
4. Upload the ESP32-CAM sketch.
5. Reconnect the UART. The LCD should show live T / H / S / L values, and the dashboard should say *AUTO mode*.
6. Measure every MOSFET gate with a multimeter. It should read **< 0.5 V**, meaning the pull-downs are working.
7. Connect 12 V, then test the pump, fans and lights **one at a time** from the dashboard.

### Voltage checkpoints

| Test point | Expected | If wrong |
|---|---|---|
| 12 V rail → GND | 11.8 – 12.2 V | Check the PSU connections |
| 5 V rail → GND | 4.8 – 5.2 V | Check the 5 V supply / USB |
| ESP32 3.3 V pin → GND | 3.2 – 3.4 V | Power issue |
| MOSFET gate (idle) | < 0.5 V | 10 kΩ pull-down missing |
| UART divider midpoint | 3.2 – 3.4 V | Check the 1 kΩ / 2 kΩ values |
