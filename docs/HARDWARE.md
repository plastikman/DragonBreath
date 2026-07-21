# Panda Breath V1.0.1 — hardware map

BIGTREETECH Panda Breath V1.0.1 control board. Mains-powered PTC chamber heater
+ AC blower fan, controlled over WiFi. Reverse-engineered from the board, the
schematic, and the stock firmware.

## Core components
| Block | Part | Notes |
|---|---|---|
| MCU | **ESP32-C3-MINI-1-H4X** | RISC-V, 4MB flash, WiFi+BLE |
| USB-UART | **CH340K** | flashing/console path (native USB unusable — see below) |
| PSU | **Hi-Link HLK-PM01** | 100–240 VAC → 5 V 0.6 A → AMS1117-3.3 |
| Heater switch | **MGR GJ-5-L SSR** | 5 A 24–380 VAC, DC-controlled, on/off only |
| Fuse | **jdtfuse T6.3A 250V** | over-current only (NOT over-temp) |
| Fan | **STKJ ST7530B220H** | ~220 VAC EC blower, 0.054 A (~12 W), 2-wire |
| Fan drive | **MOC3021 + BT136-800E** | random-phase opto-triac + TRIAC, phase-angle |
| Zero-cross | **MB6S + TLP785 (P785)** | bridge + opto → ZCD pulse to the MCU |

## GPIO map (see `components/pb_board/include/pb_board.h`)
| Function | GPIO | Confidence |
|---|---|---|
| Heater SSR (RLY_MOSFET) | 18 | INFERRED (pad 26) |
| Fan TRIAC gate | 3 | CONFIRMED (IO03) |
| Zero-cross detect | 7 | CONFIRMED (IO07; shared w/ K1 btn) |
| Chamber NTC (TH0) | 0 / ADC1_CH0 | INFERRED (pad 12; shared w/ K2 btn) |
| PTC element NTC (TH1) | 1 / ADC1_CH1 | INFERRED (pad 13) |
| Rref divider strap | 19 | reads 82 vs 33 kΩ series resistor |
| Button LEDs K1/K2/K3 | 6 / 5 / 4 | CONFIRMED |
| Buttons K2/K3 | 0 / 2 | (not implemented) |
| Console UART0 TX/RX | 21 / 20 | CONFIRMED |

> **Continuity-test the INFERRED pins before the first flash.** A wrong heater or
> TRIAC pin is a safety issue.

## Flashing path
The ESP32-C3's *native* USB-Serial-JTAG is on GPIO18/19; GPIO18 drives the heater
SSR, so native USB is unavailable. Flash via the **CH340K UART bridge** (USB-C →
CH340K → UART0) in download mode, using esptool. The stock 2nd-stage bootloader
is not to be modified; the flashing strategy (keep it vs. full own-image flash)
is a TODO in `partitions.csv`.

## Temperature sensing
See [`NTC_CONVERSION.md`](NTC_CONVERSION.md) for the fully reverse-engineered
ADC→temperature conversion (resistance divider + 114-entry R/T lookup table).
