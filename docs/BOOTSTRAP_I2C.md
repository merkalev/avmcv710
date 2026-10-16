# CV710 Bootstrap I2C Register Reference (All 696 Writes Decoded)

This document provides a complete breakdown of every I2C register transaction in `avmcv710-bootstrap.h`.

All transactions use either Opcode `0x01` (I2C Bus 0) or Opcode `0x03` (I2C Bus 1).

---

## 1. I2C Bus & Device Addressing Map

| 7-bit Address | 8-bit Addr | Device | Physical Bus | Role |
|:---:|:---:|---|:---:|---|
| `0x74` | `0xE8` | NXP/TI PCA9539 | Bus 0 | 16-bit GPIO expander (power rails, resets) |
| `0x18` | `0x30` | CVX8897 | Bus 1 | Hardware frame-rate converter ASIC |
| `0x20` | `0x40` | ADV7604 Main | Bus 0 | Analog Devices ADV7604 IO / Core map |
| `0x22` | `0x44` | ADV7604 CP | Bus 0 | ADV7604 Component Processor / Color Matrix |
| `0x26` | `0x4C` | ADV7604 KSV | Bus 0 | ADV7604 Repeater / EDID controller |
| `0x32` | `0x64` | ADV7604 VDP | Bus 0 | ADV7604 VBI Data Processor |
| `0x34` | `0x68` | ADV7604 HDMI | Bus 0 | ADV7604 HDMI Receiver Core & Equalizer |
| `0x36` | `0x6C` | ADV7604 EDID | Bus 0 | ADV7604 256-byte EDID SRAM |
| `0x10` | `0x20` | Lattice ECP3 | Bus 0 | FPGA DMA serializer & work-mode control |

---

## 2. PCA9539 GPIO Expander (Slave `0x74`)

42 writes to initialize power rails and bring components out of hardware reset:
- `reg 0x06 = 0x00`: Configuration Port 0 (all 8 pins set as outputs).
- `reg 0x07 = 0x09`: Configuration Port 1 (pins 0 and 3 are inputs, all others outputs).
- `reg 0x02 = 0x31` -> `0x33` -> `0x71`:
  - Bit 0: Power rail 1 enable (VDD 1.8V)
  - Bit 1: Power rail 2 enable (VDD 3.3V)
  - Bit 2: ADV7604 active-low hardware reset
  - Bit 3: CVX8897 output buffer enable
  - Bit 4: CVX8897 active-low hardware reset pulse
  - Bit 6: FPGA auxiliary reset line

---

## 3. CVX8897 Frame-Rate Converter (I2C Bus 1, Slave `0x18`)

18 writes via Opcode `0x03` to program default 1:1 passthrough configuration:
- `0x03 = 0x90`, `0x04 = 0x74`, `0x05 = 0x13`: PLL clock multipliers and input pixel clock divider.
- `0x06 = 0xDC`, `0x08 = 0xC0`, `0x09 = 0x30`: Bus timing and phase adjustment.
- `0x0B = 0x01`: Output format enable (passthrough mode).
- `0x0C = 0x00`: Conversion bypass enable.
- `0x0F = 0x0D`, `0x10 = 0x0D`: Internal FIFO buffer thresholds.
- `0x13 = 0x1C`, `0x15 = 0x78`, `0x16 = 0x1C`, `0x18 = 0x78`: Output drive strengths and slew rates.
- `0x8A = 0x02`, `0x0D = 0xA4`: SetAllOutputMode (passthrough native rate).

---

## 4. ADV7604 HDMI Receiver (Slaves `0x20`, `0x22`, `0x26`, `0x32`, `0x34`)

### A. Main IO Map (`0x20`, 521 writes):
- `0xF4 = 0x80`: CP map -> I2C slave 0x22
- `0xF8 = 0x4C`: Repeater/KSV map -> I2C slave 0x26
- `0xF9 = 0x64`: VDP map -> I2C slave 0x32
- `0xFA = 0x68`: HDMI map -> I2C slave 0x34
- `0xFB = 0x6C`: EDID RAM map -> I2C slave 0x36
- `0x00 = 0x08`: Video standard = HDMI-GR (graphics / HDMI auto-detect)
- `0x01 = 0x06`: Primary mode = HDMI-GR
- `0x02 = 0xFC`: Power-up all analog/digital cores (ADC, PLL, IO, CP)
- `0x03 = 0x80`: Output drive strength selection
- `0x05 = 0x2C`: Auto sync polarity, free run enable
- `0x06 = 0xA6`: LLC pixel clock delay / invert
- `0x0B = 0x44`: Tristate audio output pins
- `0x0C = 0x40`: Tristate video output pads
- `0x14 = 0x7F`, `0x15 = 0xB0`, `0x16 = 0x43`: Slew rate and drive strength
- `0x62 = 0x00`, `0x67 = 0x00`, `0x6C = 0x00`: Interrupt masks disabled

### B. HDMI Map (`0x34`, 28 writes):
- `0x00 = 0x00`: Clear HDMI reset
- `0x01 = 0x78`: Enable internal 50-ohm TMDS termination resistors
- `0x02 = 0x00`: TMDS equalizer manual override off (auto-adaptive mode)
- `0x03 = 0x18`: Automatic equalizer gain limit
- `0x0D = 0x84`: Audio error filter enable, mute on packet error
- `0x3D = 0x10`, `0x3E = 0x39`, `0x4E = 0x3B`: ADI `RxPllInitTable` (TMDS clock PLL loop filter and charge pump)
- `0x41 = 0x40`: Deep color FIFO reset
- `0x5A = 0x08`: Audio FIFO reset and clock recovery loop filter
- `0x9D = 0x00`: ADI `RxEqInitTable` (Equalizer adaptation parameter)

### C. Component Processor / Color Matrix Map (`0x22`, 43 writes):
- `0xCF = 0x01`: CP software reset
- `0x85 = 0x03`: Automatic color space conversion mode
- `0x68 = 0xF0`, `0x69 = 0x30`: Color space conversion matrix coefficients
- `0x3A = 0x80`, `0x3B = 0x80`, `0x3C = 0x00`, `0x3E = 0x80`, `0x41 = 0x0A`: Clamp, AGC and DC restoration
- `0x7C = 0xC0`, `0x7D = 0x00`: Hue offset
- `0x8B = 0x40`, `0x91 = 0x50`: Contrast and saturation multiplier
- `0x47 = 0x00`, `0x8F = 0x00`: Manual gain override registers

### D. Repeater & VDP Maps (`0x26`, `0x32`, 21 writes):
- `0x26/0x01 = 0x2E`, `0x26/0x02 = 0x00`, `0x26/0x0C = 0x1F`, `0x26/0x12 = 0x7B`, `0x26/0x15 = 0x06`: EDID HPD control and repeater state machine.
- `0x32/0x40 = 0x80`: VBI Data Processor enable.
- `0x32/0x70`..`0x77`: CGMS / WSS data decode configuration.

---

## 5. EDID RAM Upload (Slave `0x36`)

10 writes (8 blocks of 32 bytes) writing the 256-byte standard capture EDID:
- **Block 0 (0x00–0x7F):** CEA-861 Base EDID block:
  - Vendor ID: `AVM` (0x0696)
  - Product Code: `0x4478` (`ExtremeCap U3`)
  - Serial Number: 1
  - Preferred Timing: `1920x1080@60Hz` (148.5 MHz pixel clock)
  - Established Timings: 1280x720@60Hz, 720x576@50Hz, 720x480@60Hz, 640x480@60Hz
- **Block 1 (0x80–0xFF):** CEA-861 Extension block:
  - Video Data Block (SVDs): 1080p60, 1080p50, 1080i60, 1080i50, 720p60, 720p50, 480p60, 576p50
  - Audio Data Block: Linear PCM, 2 channels, 32 / 44.1 / 48 kHz, 16/20/24-bit
  - Speaker Allocation: Front Left / Front Right (Stereo)

---

## 6. Lattice ECP3 FPGA (Slave `0x10`)

29 writes to set up the high-speed pixel DMA serializer:
- `0x05 = 0x00`: Park FPGA in idle mode during bootstrap.
- `0x01 = 0x0C`, `0x02 = 0x00`: Packet burst size (512-word / 2048-byte bursts to match FX3 SuperSpeed EP `0x83` max packet group).
- `0x06 = 0x00`, `0x07 = 0x00`: FIFO clear / reset pointers.
- `0x80 = 0x22`, `0x81 = 0x00`, `0x83 = 0x01`, `0x86 = 0x11`: Internal serialization clocks and YUY2 packer timing.
