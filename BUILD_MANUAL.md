# dmr_monitor — Build and Operation Manual

**Version:** 0.4.0  
**Target platform:** Ubuntu 20.04 LTS or later, x86-64  
**Tested hardware:** Advantech ARK-3510 (Intel Core, 16 GB RAM, Ubuntu 20+)  
**SDR hardware:** RTL-SDR dongle (RTL2832U chipset)  
**Target band:** 446.002 – 446.196 MHz (PMR446 DMR, 16 × 12.5 kHz channels)

---

## Table of Contents

1. [Overview](#1-overview)
2. [System Requirements](#2-system-requirements)
3. [Hardware Setup](#3-hardware-setup)
4. [Operating System Preparation](#4-operating-system-preparation)
5. [Dependency Installation](#5-dependency-installation)
6. [RTL-SDR Driver and Blacklist](#6-rtl-sdr-driver-and-blacklist)
7. [Cloning / Copying the Source Tree](#7-cloning--copying-the-source-tree)
8. [Project Structure](#8-project-structure)
9. [Build Instructions](#9-build-instructions)
10. [Runtime Configuration](#10-runtime-configuration)
11. [Running the Monitor](#11-running-the-monitor)
12. [Output Format](#12-output-format)
13. [Tuning and Troubleshooting](#13-tuning-and-troubleshooting)
14. [Signal Processing Pipeline Reference](#14-signal-processing-pipeline-reference)
15. [Known Limitations and Caveats](#15-known-limitations-and-caveats)
16. [Extending the Pipeline](#16-extending-the-pipeline)

---

## 1. Overview

`dmr_monitor` is a passive DMR metadata extraction pipeline. It captures a 250 kHz wide IQ stream from a single RTL-SDR dongle, channelizes it into up to 16 individual 12.5 kHz DMR channels, demodulates each channel independently, decodes the Layer 2 burst structure, and extracts the following from every transmission:

- Source radio ID (24-bit)
- Destination talkgroup or radio ID (24-bit)
- Group call flag
- GPS coordinates (when a MOTOTRBO position report is present)

All output is written as JSON Lines to `stdout`. Diagnostic information goes to `stderr`. The binary has no GUI, no audio output, and no dependency on any voice codec.

The pipeline was designed for the 446.002 – 446.196 MHz PMR446 DMR band in the Netherlands, but is easily adapted to any band within the RTL-SDR's tuning range by changing three constants in `src/main.cpp`.

---

## 2. System Requirements

### Minimum

| Component | Minimum |
|-----------|---------|
| CPU | Dual-core x86-64, 1.6 GHz |
| RAM | 512 MB free |
| OS | Ubuntu 20.04 LTS (Focal) or later |
| USB | USB 2.0 port |
| Disk | 50 MB for build artefacts |

### Recommended (as tested)

| Component | Specification |
|-----------|--------------|
| CPU | Intel Core i5 / i7 (Advantech ARK-3510 or equivalent) |
| RAM | 16 GB |
| OS | Ubuntu 20.04 / 22.04 LTS |
| USB | USB 3.0 port (USB 2.0 is sufficient; 3.0 avoids marginal hubs) |
| Antenna | 1/4-wave vertical cut for 446 MHz, or a wideband discone |

The CPU load for the full 16-channel pipeline is negligible on any modern Intel Core processor. The heaviest module is the 129-tap FIR filter running 16 channels at 250 kSPS — approximately 500 million multiply-accumulate operations per second, comfortably handled by a single core with compiler autovectorisation (`-O3 -march=native`).

---

## 3. Hardware Setup

### RTL-SDR dongle

Any RTL2832U-based dongle works. Tested and recommended options:

- **RTL-SDR Blog V3** (rtl-sdr.com) — best frequency stability, direct-sampling mod available
- **NooElec NESDR SMArt** — good build quality, standard use
- Generic RTL2832U dongles — functional but may have higher frequency error

### Antenna

For 446 MHz, a 1/4-wave vertical antenna has a radiating element length of:

```
λ/4 = 300 / (446 × 4) = 168 mm
```

A simple ground-plane antenna with 4 × 168 mm radials works well at short to medium range. For longer range or indoor use, a discone or log-periodic covering 400–500 MHz is preferred.

Connect via SMA. If your dongle has a MCX or F connector, use the appropriate adapter. Keep the coax run as short as practical — at 446 MHz, a poor-quality RG-58 run of 10 m adds approximately 2–3 dB of loss.

### USB connection

Connect the dongle directly to a USB port on the Advantech ARK-3510, not through a hub. USB hubs introduce latency jitter that can cause ring buffer overruns at high sample rates. If a hub is unavoidable, use a powered USB 3.0 hub.

---

## 4. Operating System Preparation

### Update the system

```bash
sudo apt update && sudo apt upgrade -y
```

### Set the system clock (required for accurate JSON timestamps)

```bash
sudo timedatectl set-ntp true
timedatectl status
```

Verify that `NTP service: active` and `System clock synchronized: yes` are shown.

### USB permissions (optional but recommended)

To run `dmr_monitor` without root, add your user to the `plugdev` group:

```bash
sudo usermod -aG plugdev $USER
```

Log out and back in for this to take effect. The udev rules installed by `librtlsdr-dev` already grant `plugdev` members read/write access to RTL-SDR devices.

To verify after re-login:

```bash
groups | grep plugdev
```

---

## 5. Dependency Installation

All dependencies are available in the standard Ubuntu package repositories.

```bash
sudo apt install -y \
    build-essential \
    cmake \
    pkg-config \
    librtlsdr-dev \
    libusb-1.0-0-dev
```

Package breakdown:

| Package | Purpose |
|---------|---------|
| `build-essential` | GCC, G++, make, and standard headers |
| `cmake` | Build system generator (version ≥ 3.16 required) |
| `pkg-config` | Library discovery helper used by CMake |
| `librtlsdr-dev` | RTL-SDR headers (`rtl-sdr.h`) and shared library (`librtlsdr.so`) |
| `libusb-1.0-0-dev` | USB library that `librtlsdr` depends on |

Verify installations:

```bash
cmake --version        # should print 3.16.x or higher
dpkg -l librtlsdr-dev  # should show ii (installed)
```

---

## 6. RTL-SDR Driver and Blacklist

Ubuntu ships with the `dvb_usb_rtl28xxu` kernel module which claims the RTL2832U chip for DVB-T use. This **prevents librtlsdr from opening the device**. It must be blacklisted.

### Check if the module is loaded

```bash
lsmod | grep dvb
```

If any of the following appear, the device is being claimed by the DVB driver:

```
dvb_usb_rtl28xxu
dvb_usb_v2
dvb_core
```

### Blacklist the module

```bash
echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/rtlsdr.conf
echo 'blacklist dvb_usb_v2'       | sudo tee -a /etc/modprobe.d/rtlsdr.conf
echo 'blacklist rtl2832'           | sudo tee -a /etc/modprobe.d/rtlsdr.conf
sudo modprobe -r dvb_usb_rtl28xxu 2>/dev/null || true
sudo update-initramfs -u
```

### Verify the dongle is accessible

Plug in the dongle, then:

```bash
rtl_test -t
```

Expected output (example):

```
Found 1 device(s):
  0:  Realtek, RTL2838UHIDIR, SN: 00000001

Using device 0: Generic RTL2832U OEM
Found Rafael Micro R820T tuner
Supported gain values (29): 0.0 1.0 2.0 ...
Benchmarking E4000 second harmonic suppression...
```

If you see `usb_open error -3` or similar, either the blacklist has not taken effect (reboot) or the device lacks USB permissions (check the `plugdev` group).

To force unload without rebooting:

```bash
sudo rmmod dvb_usb_rtl28xxu 2>/dev/null
sudo rmmod dvb_usb_v2 2>/dev/null
```

---

## 7. Cloning / Copying the Source Tree

The source is provided as a flat directory. Copy or extract it to your working location:

```bash
cp -r /path/to/dmr_monitor_p4 ~/dmr_monitor
cd ~/dmr_monitor
```

The directory should contain:

```
dmr_monitor/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── bptc19696.h
│   ├── burst_sync.h
│   ├── capture.h
│   ├── channel_filter.h
│   ├── demod_chain.h
│   ├── fm_discriminator.h
│   ├── fsk_slicer.h
│   ├── lc_parser.h
│   ├── ringbuffer.h
│   ├── slot_manager.h
│   └── timing_recovery.h
└── src/
    ├── bptc19696.cpp
    ├── burst_sync.cpp
    ├── capture.cpp
    ├── channel_filter.cpp
    ├── demod_chain.cpp
    ├── fm_discriminator.cpp
    ├── fsk_slicer.cpp
    ├── lc_parser.cpp
    ├── main.cpp
    ├── ringbuffer.cpp
    ├── slot_manager.cpp
    └── timing_recovery.cpp
```

---

## 8. Project Structure

### `include/` — public headers

| Header | Phase | Description |
|--------|-------|-------------|
| `ringbuffer.h` | 1 | Lock-free SPSC circular buffer for raw IQ bytes |
| `capture.h` | 1 | RTL-SDR device ownership and async reader thread |
| `channel_filter.h` | 2 | Per-channel frequency shift + FIR LPF + decimation |
| `fm_discriminator.h` | 2 | Differential-phase FM demodulator |
| `timing_recovery.h` | 2 | Gardner TED symbol timing recovery |
| `fsk_slicer.h` | 2 | Adaptive 4FSK symbol decision → dibits |
| `demod_chain.h` | 2–4 | Aggregates the full pipeline for one channel |
| `burst_sync.h` | 3 | DMR sync word detection and burst assembly |
| `slot_manager.h` | 3 | TS1/TS2 state machine |
| `bptc19696.h` | 4 | BPTC(196,96) FEC decoder |
| `lc_parser.h` | 4 | LC header and GPS coordinate parser |

### `src/` — implementation files

Each `.cpp` file implements the corresponding header. `main.cpp` wires the full pipeline together and contains the channel frequency table.

---

## 9. Build Instructions

### Standard release build

```bash
cd ~/dmr_monitor
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

The resulting binary is at `build/dmr_monitor`.

### Debug build (with AddressSanitizer and UndefinedBehaviorSanitizer)

```bash
cd ~/dmr_monitor
mkdir -p build_debug
cd build_debug
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

The debug binary includes `-fsanitize=address,undefined`. Do not use it for production — it is approximately 3–5× slower and uses significantly more memory.

### Verifying the build

```bash
./dmr_monitor --help 2>&1 || echo "binary exists"
ldd ./dmr_monitor | grep rtlsdr
```

Expected `ldd` output line:

```
librtlsdr.so.0 => /usr/lib/x86_64-linux-gnu/librtlsdr.so.0 (0x...)
```

If `librtlsdr.so.0` shows `not found`, the shared library path is not configured. Fix with:

```bash
sudo ldconfig
```

### CMake options summary

| Variable | Default | Description |
|----------|---------|-------------|
| `CMAKE_BUILD_TYPE` | `Release` | `Release` = `-O3 -march=native`, `Debug` = `-g -O0 -fsanitize=...` |
| `CMAKE_VERBOSE_MAKEFILE` | `OFF` | Set to `ON` to see full compiler commands |

To enable verbose output:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE=ON
```

---

## 10. Runtime Configuration

All configuration is done by editing constants near the top of `src/main.cpp` and rebuilding. There are no command-line arguments in Phase 4.

### SDR hardware parameters (`CaptureConfig` in `main.cpp`)

```cpp
CaptureConfig cap_cfg;
cap_cfg.device_index  = 0;           // USB device index (0 = first dongle)
cap_cfg.centre_freq   = 446099000;   // Centre frequency in Hz (446.099 MHz)
cap_cfg.sample_rate   = 250000;      // Sample rate in S/s (250 kSPS)
cap_cfg.gain_db       = 30;          // Tuner gain in dB (manual mode)
cap_cfg.auto_gain     = false;       // true = RTL-SDR auto-gain (not recommended)
cap_cfg.ring_capacity = 4 * 1024 * 1024;  // Ring buffer size (4 MB)
```

### Gain setting

The `gain_db` value is the **tuner gain in dB**, applied to the R820T2 (or compatible) tuner front end. It controls the RF amplification before the ADC.

Start at 30 dB. If you see ring buffer overruns (printed to stderr) or the slicer `outer_lvl` is reported as very small, the signal may be clipping the ADC or too weak respectively.

Practical calibration procedure:

1. Set `gain_db = 30`. Run. Observe `outer_lvl` in the status output.
2. If `outer_lvl > 0.3` and no overruns: good, leave at 30.
3. If `outer_lvl < 0.02`: signal too weak — increase gain to 40 or 50, or check antenna.
4. If overruns appear: the ring buffer is filling faster than it is drained, meaning too much data. Reduce gain or check CPU load.

Available gain steps depend on the specific R820T2 chip. Common values (in dB): 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 37, 42, 43, 45, 47, 49, 50.

librtlsdr rounds to the nearest supported step automatically.

### Channel table (`CHANNELS[]` in `main.cpp`)

The channel table lists the absolute centre frequency and a label for each channel to monitor. Edit this array to add, remove, or change channels:

```cpp
static const ChannelDef CHANNELS[] = {
    { 446006250.0f, "446.006" },
    { 446018750.0f, "446.019" },
    // ... add or remove entries here
};
```

Rules:

- All frequencies must be within ±125 kHz of `cap_cfg.centre_freq` (i.e., within the 250 kHz capture bandwidth).
- Frequencies are the channel **centre** in Hz. DMR channels on the 12.5 kHz PMR446 raster sit at 446.006 25, 446.018 75, 446.031 25, … MHz (offset +6.25 kHz from each 12.5 kHz boundary).
- Labels are arbitrary strings — they appear in JSON output as the `ch` field.
- There is no hard limit on the number of channels. Each adds one `DemodChain` instance. On the Advantech ARK-3510, 16 channels uses a negligible fraction of one CPU core.

### Adapting to a different band

To monitor a different band:

1. Change `cap_cfg.centre_freq` to the centre of your target band in Hz.
2. Replace all entries in `CHANNELS[]` with the channel frequencies you want.
3. Verify all channel frequencies are within ±125 kHz of the new centre frequency.
4. Rebuild.

Example for a UHF commercial band centred at 450.500 MHz:

```cpp
cap_cfg.centre_freq = 450500000;

static const ChannelDef CHANNELS[] = {
    { 450450000.0f, "450.450" },
    { 450462500.0f, "450.463" },
    { 450475000.0f, "450.475" },
    { 450500000.0f, "450.500" },
    { 450525000.0f, "450.525" },
};
```

---

## 11. Running the Monitor

### Basic operation

```bash
cd ~/dmr_monitor/build
./dmr_monitor
```

JSON events appear on `stdout`. Diagnostic status appears on `stderr`. To separate them:

```bash
./dmr_monitor > events.jsonl 2>status.log
```

To monitor `stdout` and `stderr` simultaneously in separate terminal panes:

```bash
# Terminal 1 (status)
./dmr_monitor 2>&1 1>/dev/null

# Terminal 2 (events)
./dmr_monitor 1> >(cat) 2>/dev/null
```

Or more simply, redirect to both in one shot:

```bash
./dmr_monitor > events.jsonl 2>&1 &
tail -f events.jsonl
```

### Stopping cleanly

Press `Ctrl-C` or send `SIGTERM`. The pipeline performs an orderly shutdown:

1. The channelizer thread stops reading.
2. `rtlsdr_cancel_async()` unblocks the capture thread.
3. The RTL-SDR device is closed.
4. Final statistics are printed to `stderr`.

### Running as a systemd service

Create `/etc/systemd/system/dmr_monitor.service`:

```ini
[Unit]
Description=DMR metadata monitor
After=network.target

[Service]
Type=simple
User=YOUR_USERNAME
WorkingDirectory=/home/YOUR_USERNAME/dmr_monitor/build
ExecStart=/home/YOUR_USERNAME/dmr_monitor/build/dmr_monitor
StandardOutput=append:/var/log/dmr_monitor/events.jsonl
StandardError=append:/var/log/dmr_monitor/status.log
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Then:

```bash
sudo mkdir -p /var/log/dmr_monitor
sudo chown YOUR_USERNAME /var/log/dmr_monitor
sudo systemctl daemon-reload
sudo systemctl enable dmr_monitor
sudo systemctl start dmr_monitor
sudo systemctl status dmr_monitor
```

---

## 12. Output Format

### JSON Lines

Every decoded event is a single JSON object on one line, terminated by `\n`. The stream is suitable for direct ingestion by `jq`, `logstash`, `vector`, `sqlite3 --json`, or any line-oriented JSON processor.

### Voice call event

Emitted when a voice LC header burst is decoded with a valid FLCO 0x00 (group call) or 0x03 (individual call):

```json
{
  "ts":    "2026-04-08T14:23:01Z",
  "ch":    "446.006",
  "slot":  1,
  "type":  "voice",
  "src":   1234567,
  "dst":   9001,
  "group": true
}
```

| Field | Type | Description |
|-------|------|-------------|
| `ts` | string | UTC timestamp, ISO 8601 |
| `ch` | string | Channel label from the `CHANNELS[]` table |
| `slot` | integer | Timeslot (1 or 2) — see §15 on accuracy |
| `type` | string | `"voice"` |
| `src` | integer | Source radio ID (24-bit, 0–16 777 215) |
| `dst` | integer | Destination talkgroup (group=true) or radio (group=false) |
| `group` | boolean | `true` = group call, `false` = individual call |

### GPS event

Emitted when a data burst payload matches the MOTOTRBO compressed position report format:

```json
{
  "ts":   "2026-04-08T14:23:15Z",
  "ch":   "446.094",
  "slot": 2,
  "type": "gps",
  "src":  0,
  "lat":  52.373102,
  "lon":  4.892200
}
```

| Field | Type | Description |
|-------|------|-------------|
| `src` | integer | Always 0 in Phase 4 — source ID from data header not yet decoded |
| `lat` | float | Latitude in decimal degrees, WGS-84, 6 decimal places (~0.1 m resolution) |
| `lon` | float | Longitude in decimal degrees, WGS-84 |

### Processing the output with jq

Filter only voice events:
```bash
./dmr_monitor | jq 'select(.type=="voice")'
```

Extract unique source IDs seen:
```bash
./dmr_monitor | jq -r 'select(.type=="voice") | .src' | sort -u
```

Watch for a specific talkgroup:
```bash
./dmr_monitor | jq 'select(.type=="voice" and .dst==9001)'
```

Log GPS events to a CSV:
```bash
./dmr_monitor | jq -r 'select(.type=="gps") | [.ts, .ch, .lat, .lon] | @csv' >> gps_log.csv
```

### Stderr status output

Every 10 seconds, a summary is printed to `stderr`:

```
-- 10s summary --
  446.006    12 bursts/10s  ts1=VOICE  ts2=IDLE
  446.094     3 bursts/10s  ts1=IDLE   ts2=DATA
  RTL: 25.0 MB  overruns: 0
```

`overruns: 0` is the healthy state. Any non-zero overrun count means the ring buffer filled faster than the channelizer thread drained it — see §13.

---

## 13. Tuning and Troubleshooting

### No output on known-active channel

Work through the pipeline from the bottom up:

**Step 1 — Confirm the dongle sees the signal.**

```bash
rtl_power -f 446e6:446.2e6:12.5k -g 30 -i 1 sweep.csv
```

Open `sweep.csv` in a spreadsheet. You should see elevated power in the bins corresponding to active channels. If all bins are uniform (noise floor), the antenna or frequency is wrong.

**Step 2 — Confirm symbol recovery.**

In Phase 2, the status output shows `Sym/s` and `outer_lvl` per channel. Expected values on an active channel:

- `Sym/s` ≈ 4800 (±50)
- `omega` ≈ 5.0–5.4
- `outer_lvl` > 0.02

If `Sym/s` is near 4800 but `outer_lvl` is very small, the signal is present but very weak — increase gain.

If `Sym/s` is 0, the channel filter is not passing the signal — check that the channel frequency in `CHANNELS[]` matches the actual transmit frequency.

**Step 3 — Confirm burst sync.**

In Phase 3, burst events printed to `stdout` confirm sync detection. If no burst events appear despite symbol recovery, the sync patterns may not match — see §15 on BPTC and sync pattern verification.

**Step 4 — Confirm BPTC decode.**

Temporarily uncomment the generic burst output block in `make_layer2_cb()` in `main.cpp`:

```cpp
// Uncomment this block:
{
    std::lock_guard<std::mutex> lk(g_stdout_mtx);
    printf("{\"ts\":\"%s\",\"ch\":\"%s\",\"slot\":%d,"
           "\"type\":\"burst\","
           "\"burst_type\":\"%s\",\"state\":\"%s\"}\n", ...);
}
```

Rebuild. If you now see `"type":"burst"` events but no `"type":"voice"` events, the burst sync is working but BPTC is failing. Check the column correction equations in `bptc19696.cpp` (see §15).

### Ring buffer overruns

Overruns mean the channelizer thread is falling behind the capture thread.

Causes and fixes:

| Cause | Fix |
|-------|-----|
| Too many channels for CPU | Reduce NUM_CHANNELS |
| CPU under heavy load from other processes | Check `htop`, reduce load |
| USB transfer errors causing burst large reads | Use `dmesg` to check USB errors |
| Debug build in use | Use Release build (`-O3 -march=native`) |

The channelizer runs in a single thread and processes all channels sequentially. Each 2048-pair block at 250 kSPS takes approximately 8.2 ms to arrive. If processing one block takes longer than 8.2 ms, overruns occur. On the Advantech ARK-3510 with a Release build, a 16-channel pipeline processes each block in well under 1 ms.

### Frequency offset / PPM error

RTL-SDR dongles have a crystal oscillator that can be off by ±100 ppm or more. At 446 MHz, 100 ppm = 44.6 kHz offset — enough to miss a channel entirely.

Measure and correct the PPM error:

```bash
# Use a known reference signal (e.g. an FM broadcast station at known frequency)
# or the KiwiSDR network to measure the offset
rtl_test -p 60   # runs for 60 seconds, prints PPM estimate
```

Then correct in software. librtlsdr does not expose PPM correction via the C API used here, but you can compensate by adjusting `cap_cfg.centre_freq` and all channel frequencies by the PPM offset:

```
corrected_freq = nominal_freq * (1 - ppm / 1e6)
```

Example: nominal 446099000 Hz, PPM = +50:
```
corrected = 446099000 * (1 - 50/1e6) = 446099000 - 22305 = 446076695 Hz
```

### Timeslot assignment is wrong

Timeslot assignment in Phase 4 uses a simple alternating scheme and is only correct ~50% of the time without CACH decoding. This does not affect the correctness of the decoded IDs or GPS coordinates — it only affects the `slot` field in JSON output. See §15 for details and the Phase 5 extension path.

---

## 14. Signal Processing Pipeline Reference

```
RTL-SDR dongle
  │  USB 2.0
  │  IQ samples: uint8_t pairs, 250 kSPS
  │  Encoding: I = bytes[2n], Q = bytes[2n+1], DC offset at 127.5
  ▼
RingBuffer  (4 MB, lock-free SPSC)
  │  Producer: RTL-SDR async callback thread
  │  Consumer: channelizer thread
  ▼
[per 2048-pair block, for each of N channels in sequence:]
  │
  ▼ ChannelFilter
  │  1. uint8_t IQ → complex<float> in ±1.0
  │  2. Multiply by rotating phasor e^(-j·2π·Δf·n/250000)
  │     Δf = channel_freq − 446.099 MHz
  │  3. 129-tap FIR LPF (Hamming window, cutoff 5 kHz / 250 kHz = 0.02)
  │  4. Decimate ×10 → complex<float> at 25 kSPS
  │
  ▼ FmDiscriminator
  │  disc(n) = Im(z[n] · conj(z[n−1]))
  │          = Q[n]·I[n−1] − I[n]·Q[n−1]
  │  Output: float at 25 kSPS (proportional to instantaneous frequency)
  │
  ▼ TimingRecovery  (Gardner TED + PI loop filter)
  │  Nominal samples/symbol: 25000 / 4800 = 5.2083̄
  │  Error: e(k) = x(k−T/2) · (x(k) − x(k−T))
  │  Loop filter: K1=0.02 (proportional), K2=0.002 (integral)
  │  Output: float symbol value at 4800 sym/s
  │
  ▼ FskSlicer  (adaptive 4-level decision)
  │  Tracks EMA of outer symbol levels (pos_peak_, neg_peak_)
  │  Thresholds at pos_peak_/2 (high), 0 (zero), neg_peak_/2 (low)
  │  Maps symbol value → dibit:  01 (+outer)  00 (+inner)
  │                               10 (−inner)  11 (−outer)
  │
  ▼ BurstSync
  │  48-bit shift register, checked against 4 sync patterns per dibit
  │  Hamming distance threshold ≤ 4 bits
  │  On match: recovers 49 first-half dibits from 128-entry history buffer
  │  Collects 49 second-half + 10 guard dibits
  │  Emits Burst (98 payload dibits + metadata)
  │
  ▼ SlotManager  (TS1 / TS2 state machine)
  │  Alternating burst → timeslot assignment
  │  States: IDLE → VOICE / DATA → IDLE
  │  Emits SlotEvent (burst + state + count)
  │
  ▼ BPTC19696
  │  98 dibits → 196 bits
  │  Deinterleave: out[k] = in[(k×13) mod 196]
  │  Arrange: 13×15 matrix (row-major)
  │  Row Hamming(15,11): correct rows 0–8
  │  Column Hamming(13,9): correct columns 0–14
  │  Second row pass
  │  Extract: rows 0–8, cols 0–10 → 96 LC bits
  │
  ▼ LcParser
  │  Voice: bits[2:7]=FLCO, bits[24:47]=DstID, bits[48:71]=SrcID
  │  GPS:   bytes 1–6 from 96 bits → signed 24-bit lat/lon
  │
  ▼ stdout (JSON Lines)
```

---

## 15. Known Limitations and Caveats

### BPTC column correction equations

The Hamming(13,9) column correction in `bptc19696.cpp` uses equations derived from a shortened Hamming(15,11) code. These have not been validated against a real radio transmitter. If BPTC decode fails on strong signals where the Phase 3 burst sync is clearly working, bypass the column correction step (comment out the column correction loop in `decode()`) and test if LC decoding improves. Row-only Hamming(15,11) is sufficient for BER below approximately 1%.

### Sync pattern constants

The four 48-bit sync patterns in `burst_sync.h` (`kSyncBsVoice`, `kSyncMsVoice`, `kSyncBsData`, `kSyncMsData`) are widely cited in SDR community documentation and match the values used in DSD-FME. They should be verified against ETSI TS 102 361-1 Table 9.11. If burst detection rate is zero on a known-active channel with confirmed symbol recovery, test each pattern against a known capture.

### Timeslot assignment

Proper TS1/TS2 demultiplexing requires either a frame timing reference from the base station or CACH (Common Announcement CHannel) decoding. The Phase 4 implementation uses simple burst alternation: odd bursts = TS1, even = TS2. This is structurally correct for continuous traffic but will flip on the first missed burst. The `slot` field in JSON output is an approximation. It does not affect the decoded IDs or GPS.

### GPS source ID is always 0

Extracting the Source ID from a GPS event requires parsing the Short Data Header burst that precedes the position report block. The header arrives in a separate burst and requires multi-burst state tracking. This is not implemented in Phase 4. All GPS events emit `"src":0`. The Source ID for GPS transmissions can be correlated from prior voice events on the same channel and timeslot.

### GPS format variations

The GPS parser targets the MOTOTRBO "immediate position response" format (packet type byte 0x00 or 0x20). Other GPS formats encountered in the field include:

- ETSI LRRP compressed position (slightly different coordinate encoding)
- Motorola enhanced location with heading and velocity
- Third-party radio vendor extensions

If coordinates appear systematically offset, the scaling constant (currently `90.0 / 0x7FFFFF` for latitude) may need adjustment. Compare decoded values against a GPS ground truth from the same radio.

### Encrypted traffic

AES-256 encrypted DMR traffic produces valid burst sync matches and valid BPTC decodes, but the LC payload will be garbage. The `parse_voice_lc()` function will return `valid=false` for most encrypted frames due to the FLCO validity check. No decryption is attempted or implemented.

---

## 16. Extending the Pipeline

### Phase 5 extension points

The codebase is structured so each phase adds modules without breaking earlier ones. Natural Phase 5 extensions in order of usefulness:

**GPS Source ID from data header**  
Parse the Short Data Header burst to extract the source radio ID. The header uses the same BPTC decode path. Match it to the subsequent data block by tracking a per-channel, per-timeslot "pending header" state in `SlotManager`.

**CACH decoding for proper timeslot assignment**  
The CACH (Common Announcement CHannel) field occupies the first 12 bits of each burst. Decoding it gives the correct TS1/TS2 assignment and eliminates the alternating-burst approximation.

**Database output**  
Replace the `fprintf(stdout, ...)` calls in `make_layer2_cb()` with inserts to SQLite (via `libsqlite3-dev`) or forwarding to a MQTT broker. The JSON Lines format makes this straightforward — the JSON is already structured for ingestion.

**Multiple RTL-SDR dongles**  
The `Capture` class owns one device. Instantiating multiple `Capture` objects with different `device_index` values and different `centre_freq` settings allows monitoring of wider bands than 250 kHz. Each dongle requires its own channelizer thread.

**RS(12,9) Reed-Solomon on LC**  
The 24 bits in LC output positions 72–95 are Reed-Solomon parity for multi-burst LC fields (voice header, embedded LC, terminator). Implementing RS(12,9) over GF(2^9) would recover LC data from frames with more than 1 burst error. Only relevant in very poor channel conditions.

### Adding a new output format

All output goes through the lambda in `make_layer2_cb()` in `main.cpp`. To add a new format (for example, plain text or CSV), modify that function. The `LcResult` and `GpsResult` structs contain all parsed fields.

---

*Document version: 1.0 — 2026-04-08*
