# dmr_monitor — Build and Operation Manual

**Version:** 0.5.0  
**Target platform:** Ubuntu 20.04 LTS or later, x86-64  
**Tested hardware:** Advantech ARK-3510 (Intel Core, 16 GB RAM, Ubuntu 20+)  
**SDR hardware:** RTL-SDR dongle (RTL2832U chipset); KrakenSDR coexistence documented in §6 and §18  
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
17. [UDP Output and Node-RED Integration](#17-udp-output-and-node-red-integration)
18. [KrakenSDR Coexistence and Device Serial Selection](#18-krakenSDR-coexistence-and-device-serial-selection)
19. [Running Multiple Instances — Seven-Service Deployment](#19-running-multiple-instances--seven-service-deployment)

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

Ubuntu ships with the `dvb_usb_rtl28xxu` kernel module which claims the RTL2832U chip for DVB-T use. This **prevents librtlsdr from opening the device**. The module must be blacklisted before `dmr_monitor` can open the dongle.

### If you run a KrakenSDR — read this first

The KrakenSDR contains five RTL2832U chips on a single board. Its installer almost certainly already blacklisted the DVB modules. Applying the blacklist a second time is harmless, but there is no need. Check first:

```bash
cat /etc/modprobe.d/*.conf | grep dvb
ls /etc/udev/rules.d/ | grep -iE 'rtl|kraken'
```

If you see lines containing `blacklist dvb_usb_rtl28xxu` anywhere in `/etc/modprobe.d/`, the blacklist is already in place. Skip to the "Verify the dongle is accessible" step below.

If you have a KrakenSDR and multiple dongles, also read §18 before continuing — device index conflicts are a more likely problem than the blacklist.

### What the blacklist does and what it affects

The blacklist is a global instruction to the kernel module loader: never load the named drivers, regardless of which device triggers them. It affects every RTL2832U chip on the machine.

| Use case | Effect of blacklist |
|----------|-------------------|
| RTL-SDR dongles used with librtlsdr apps (GQRX, dump1090, rtl_433, dmr_monitor) | None — these apps use librtlsdr directly, not the DVB driver |
| KrakenSDR (uses librtlsdr internally) | None — same as above |
| RTL2832U dongle used as a DVB-T television tuner | **Breaks completely** — the DVB driver is removed |
| HackRF, USRP, Airspy, SDRplay, LimeSDR | None — different chipsets, different drivers |

If none of your dongles are used for DVB-T television reception, blacklist without concern. If any dongle is used as a TV tuner, use the per-device unbind approach described at the end of this section instead.

### Check if the module is currently loaded

```bash
lsmod | grep dvb
```

If any of the following appear, the DVB driver is active and claiming RTL2832U devices:

```
dvb_usb_rtl28xxu
dvb_usb_v2
dvb_core
```

### Apply the blacklist

```bash
echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/rtlsdr.conf
echo 'blacklist dvb_usb_v2'       | sudo tee -a /etc/modprobe.d/rtlsdr.conf
echo 'blacklist rtl2832'           | sudo tee -a /etc/modprobe.d/rtlsdr.conf
sudo modprobe -r dvb_usb_rtl28xxu 2>/dev/null || true
sudo update-initramfs -u
```

`update-initramfs -u` rebuilds the early-boot RAM filesystem so the blacklist persists across kernel updates. Without it the blacklist may not survive certain kernel upgrade cycles.

### Verify the dongle is accessible

Plug in the monitoring dongle, then:

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

If you see `usb_open error -3`, either the blacklist has not taken effect (reboot) or the device lacks USB permissions (check the `plugdev` group).

To force unload without rebooting:

```bash
sudo rmmod dvb_usb_rtl28xxu 2>/dev/null
sudo rmmod dvb_usb_v2 2>/dev/null
```

### Alternative: per-device unbind (if you need DVB-T on another dongle)

If you need to keep DVB-T working on a different dongle, do not blacklist system-wide. Instead, unbind the DVB driver from only the monitoring dongle by its USB bus path:

```bash
# Find the USB path of the monitoring dongle
dmesg | grep dvb_usb_rtl28xxu | tail -5
# Look for a line like: usb 1-1.2: dvb_usb_rtl28xxu ...
# The path is the part after 'usb ' — e.g. 1-1.2

echo '1-1.2' | sudo tee /sys/bus/usb/drivers/dvb_usb_rtl28xxu/unbind
```

This unbind is not persistent — it must be reapplied after every reboot or device reconnect. To automate it, add an `ExecStartPre` line to the systemd service unit:

```ini
ExecStartPre=/bin/sh -c 'echo $(cat /sys/bus/usb/devices/*/product 2>/dev/null | grep -l "RTL" | head -1 | xargs dirname | xargs basename) > /sys/bus/usb/drivers/dvb_usb_rtl28xxu/unbind 2>/dev/null || true'
```

The blacklist approach is simpler and more reliable if DVB-T is not needed.

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
cap_cfg.device_index  = 0;           // USB device index (0 = first dongle found)
cap_cfg.serial        = "";          // Serial string — if non-empty, overrides device_index
cap_cfg.centre_freq   = 446099000;   // Centre frequency in Hz (446.099 MHz)
cap_cfg.sample_rate   = 250000;      // Sample rate in S/s (250 kSPS)
cap_cfg.gain_db       = 30;          // Tuner gain in dB (manual mode)
cap_cfg.auto_gain     = false;       // true = RTL-SDR auto-gain (not recommended)
cap_cfg.ring_capacity = 4 * 1024 * 1024;  // Ring buffer size (4 MB)
```

If you have multiple RTL-SDR devices on the same machine (including a KrakenSDR), use the `serial` field to select the monitoring dongle by its programmed serial number rather than by USB enumeration index. See §18 for how to set and use a serial number.

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

### Running as a single systemd service

For a single instance, create `/etc/systemd/system/dmr_monitor.service`:

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

For running multiple instances simultaneously — e.g. five instances on a KrakenSDR plus two Inmarsat receivers — see §19.

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

---

## 17. UDP Output and Node-RED Integration

### Overview

Every decoded JSON event is sent as a single UDP datagram to a configurable host and port, in addition to being written to `stdout`. The two destinations are independent — if UDP fails (e.g. Node-RED is not running), the service continues and stdout output is unaffected.

### Configuration

Two constants at the top of `src/main.cpp`:

```cpp
static constexpr const char*  UDP_HOST = "127.0.0.1";  // destination host
static constexpr uint16_t     UDP_PORT = 41414;         // destination port
```

Change these and rebuild. If Node-RED runs on the same machine as the monitor (the common case on the Advantech ARK-3510), leave `127.0.0.1`. If Node-RED runs on another machine on the same network, replace with its IP address.

Port `41414` is the Node-RED UDP input node default. Any unused UDP port above 1024 works.

### Build

No new dependencies. POSIX sockets are part of the standard C library on Ubuntu.

```bash
cd ~/dmr_monitor
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Startup output

On successful start you will see on `stderr`:

```
[udp] output open — sending to 127.0.0.1:41414
[dmr_monitor] running
  JSON  -> stdout
  JSON  -> UDP 127.0.0.1:41414
  Press Ctrl-C to stop
```

If Node-RED is not running when the monitor starts, UDP open still succeeds (UDP is connectionless — there is no handshake). Datagrams sent while Node-RED is down are silently discarded by the OS. When Node-RED starts or restarts, it begins receiving subsequent datagrams immediately with no action required on the monitor side.

### Node-RED flow setup

#### Step 1 — UDP input node

Drag a **udp in** node onto the canvas and configure it:

| Setting | Value |
|---------|-------|
| Listen for | UDP messages |
| Port | 41414 |
| Output | a String |
| IP address | 0.0.0.0 (listen on all interfaces) |

Set "Output" to **a String** (not a Buffer). This delivers `msg.payload` as a JavaScript string, ready to parse.

#### Step 2 — JSON parse node

Connect **udp in** → **json** node. This converts the JSON string to a JavaScript object. After this node, `msg.payload` is an object with fields `ts`, `ch`, `slot`, `type`, `src`, `dst` (voice) or `lat`, `lon` (GPS).

#### Step 3 — Route by event type

Connect the **json** node to a **switch** node:

| Rule | Condition | Route to |
|------|-----------|---------|
| 1 | `msg.payload.type == "voice"` | Voice processing branch |
| 2 | `msg.payload.type == "gps"` | GPS / map branch |

#### Step 4 — Example: voice call dashboard

After the switch (voice branch):

```
[switch] → [change: set msg.payload to msg.payload]
         → [ui_text: "Last caller: {{payload.src}} → {{payload.dst}}"]
         → [ui_table: columns src, dst, group, ts, ch]
```

#### Step 5 — Example: GPS on a world map

After the switch (GPS branch):

Connect to a **worldmap** node (install `node-red-contrib-web-worldmap`):

```javascript
// In a [function] node before worldmap:
msg.payload = {
    name:  "Radio " + msg.payload.src,
    lat:   msg.payload.lat,
    lon:   msg.payload.lon,
    icon:  "walkie-talkie",
    label: msg.payload.ch + " TS" + msg.payload.slot
};
return msg;
```

This places a map marker for each GPS report. Markers update in place when a radio reports from a new location.

#### Minimal working flow (import this JSON into Node-RED)

```json
[
  {
    "id": "udp_in",
    "type": "udp in",
    "name": "DMR monitor",
    "port": "41414",
    "datatype": "utf8",
    "multicast": "false",
    "group": "",
    "iface": "",
    "wires": [["json_parse"]]
  },
  {
    "id": "json_parse",
    "type": "json",
    "name": "",
    "property": "payload",
    "action": "obj",
    "pretty": false,
    "wires": [["type_switch"]]
  },
  {
    "id": "type_switch",
    "type": "switch",
    "name": "Route by type",
    "property": "payload.type",
    "propertyType": "msg",
    "rules": [
      {"t": "eq", "v": "voice", "vt": "str"},
      {"t": "eq", "v": "gps",   "vt": "str"}
    ],
    "wires": [["voice_debug"], ["gps_debug"]]
  },
  {
    "id": "voice_debug",
    "type": "debug",
    "name": "Voice events",
    "active": true,
    "tosidebar": true,
    "wires": []
  },
  {
    "id": "gps_debug",
    "type": "debug",
    "name": "GPS events",
    "active": true,
    "tosidebar": true,
    "wires": []
  }
]
```

Import via Node-RED menu → Import → paste the JSON above. This gives you a working receiver with debug output in the sidebar. Replace the **debug** nodes with dashboard or database nodes as needed.

### Testing UDP delivery without Node-RED

To confirm datagrams are arriving before Node-RED is set up, use `netcat`:

```bash
nc -ulp 41414
```

Start the monitor in a second terminal. Each decoded event should appear in the `nc` window as a JSON line.

Alternatively, use `socat`:

```bash
socat UDP-RECV:41414 STDOUT
```

### Sending to a remote Node-RED instance

If Node-RED runs on a different machine (e.g. a separate server or a Raspberry Pi on the same LAN):

1. Change `UDP_HOST` in `main.cpp` to the Node-RED machine's IP address.
2. Ensure the firewall on the Node-RED machine allows inbound UDP on port 41414.
3. On Ubuntu with `ufw`:

```bash
sudo ufw allow 41414/udp
```

There is no authentication on UDP — do not expose port 41414 to an untrusted network without additional controls.

### 10-second status line

The UDP connection state is included in the periodic status summary on `stderr`:

```
  RTL: 25.0 MB  overruns: 0  UDP: ok
```

`UDP: ok` means the socket is open and sendto() is succeeding. `UDP: closed` means open() failed at startup — check the host/port and rebuild.

---

## 18. KrakenSDR Coexistence and Device Serial Selection

### The problem

`librtlsdr` enumerates all RTL2832U chips found on the system and assigns them integer indices (0, 1, 2, …) in USB enumeration order. This order is determined by the kernel at boot and is **not deterministic** across reboots, USB hub topology changes, or device reconnects.

A KrakenSDR exposes five RTL2832U chips. If the KrakenSDR is connected alongside a monitoring dongle, `librtlsdr` will enumerate all six devices. The monitoring dongle may be assigned index 0, 3, or any other value depending on which USB port it is connected to and the order devices are claimed.

`dmr_monitor` defaults to `device_index = 0`. This will open whichever device the OS happens to assign index 0 — which may be one of the KrakenSDR's five chips, not the monitoring dongle. The result is either an open failure (KrakenSDR holds the device) or incorrect RF input (wrong antenna, wrong frequency response).

### The solution — serial number selection

Every RTL-SDR dongle has a 256-byte EEPROM that stores, among other things, a programmable serial number string. By assigning a unique serial to the monitoring dongle and selecting by serial rather than index, device selection becomes completely robust regardless of enumeration order or KrakenSDR presence.

This is a one-time hardware configuration step. The serial is stored in the dongle's EEPROM and persists across reboots, reconnects, and USB topology changes.

### Step 1 — Identify the monitoring dongle

Disconnect the KrakenSDR temporarily. With only the monitoring dongle connected:

```bash
rtl_test -t
```

Note the device index shown (will be 0 if it is the only device). Also note the current serial number shown in the output line:

```
0:  Realtek, RTL2838UHIDIR, SN: 00000001
```

`SN: 00000001` is the current serial. Many cheap dongles ship with `00000001` or `0` — not unique enough to rely on.

### Step 2 — Write a unique serial to the monitoring dongle

```bash
rtl_eeprom -d 0 -s dmr_monitor
```

`-d 0` selects device index 0 (the dongle, while KrakenSDR is disconnected).  
`-s dmr_monitor` sets the serial string to `dmr_monitor`.

You will be prompted to confirm the write:

```
Current configuration:
...
Serial number:      00000001
...
Write new configuration to device [y/n]? y
```

After writing, unplug and replug the dongle. Verify:

```bash
rtl_test -d dmr_monitor
```

Expected output:

```
Found 1 device(s) with serial 'dmr_monitor':
  0:  Realtek, RTL2838UHIDIR, SN: dmr_monitor
```

### Step 3 — Update `capture.h` to support serial selection

The `CaptureConfig` struct needs a `serial` field. Replace `include/capture.h` with the version from Phase 5 (already updated), or add the field manually:

```cpp
struct CaptureConfig {
    uint32_t    device_index  = 0;
    std::string serial        = "";   // if non-empty, overrides device_index
    uint32_t    centre_freq   = 446'099'000;
    uint32_t    sample_rate   = 250'000;
    int         gain_db       = 30;
    bool        auto_gain     = false;
    size_t      ring_capacity = 4 * 1024 * 1024;
};
```

### Step 4 — Update `capture.cpp` to resolve by serial

In `capture.cpp`, replace the `rtlsdr_open` call in `start()`:

```cpp
// Old (index only):
if (rtlsdr_open(&dev, cfg_.device_index) < 0) {
    throw std::runtime_error("Capture: failed to open RTL-SDR device index "
                              + std::to_string(cfg_.device_index));
}

// New (serial takes priority if set):
uint32_t open_index = cfg_.device_index;
if (!cfg_.serial.empty()) {
    const int idx = rtlsdr_get_index_by_serial(cfg_.serial.c_str());
    if (idx < 0) {
        throw std::runtime_error(
            "Capture: no RTL-SDR device with serial '" + cfg_.serial + "' found");
    }
    open_index = static_cast<uint32_t>(idx);
    fprintf(stderr, "[capture] serial '%s' resolved to device index %u\n",
            cfg_.serial.c_str(), open_index);
}
if (rtlsdr_open(&dev, open_index) < 0) {
    throw std::runtime_error("Capture: failed to open RTL-SDR device index "
                              + std::to_string(open_index));
}
```

### Step 5 — Set the serial in `main.cpp`

```cpp
cap_cfg.device_index = 0;       // fallback if serial not found
cap_cfg.serial       = "dmr_monitor";  // takes priority
```

Rebuild:

```bash
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Step 6 — Reconnect KrakenSDR and verify

Reconnect the KrakenSDR. Run:

```bash
rtl_test -t
```

All six devices will be listed. Note which index the monitoring dongle has been assigned — it may no longer be 0. Then start `dmr_monitor`:

```bash
./dmr_monitor
```

You should see on `stderr`:

```
[capture] serial 'dmr_monitor' resolved to device index 3
[capture] started — freq=446.099 MHz  SR=250 kSPS  gain=30 dB
```

The index may be any value — that is expected and correct. The serial lookup handles it transparently.

### Verifying KrakenSDR is undisturbed

After starting `dmr_monitor`, confirm the KrakenSDR software still operates normally. Since `dmr_monitor` opens exactly one device by serial, the other five RTL2832U chips remain available to the KrakenSDR stack.

If the KrakenSDR uses SoapySDR, you can verify device availability:

```bash
SoapySDRUtil --find="driver=rtlsdr"
```

This should list all six devices. The one claimed by `dmr_monitor` will show as busy if you try to open it again, but the other five will be accessible.

### Summary of changes for KrakenSDR coexistence

| What | Action |
|------|--------|
| DVB blacklist | Check if KrakenSDR installer already did it — likely yes |
| Device index | Do not rely on it — use serial selection instead |
| Monitoring dongle serial | Set once with `rtl_eeprom -d 0 -s dmr_monitor` |
| `capture.h` | Add `std::string serial = ""` to `CaptureConfig` |
| `capture.cpp` | Add serial-to-index resolution before `rtlsdr_open` |
| `main.cpp` | Set `cap_cfg.serial = "dmr_monitor"` |

---

## 19. Running Multiple Instances — Seven-Service Deployment

This section documents the full seven-instance deployment using a KrakenSDR (five chips) and two NooElec dongles (Inmarsat), all running simultaneously on the Advantech ARK-3510.

### Architecture overview

```
KrakenSDR (1 USB 3.0 cable)
  ├── chip 0  (serial: kraken_ch0)  →  dmr_monitor instance 1
  ├── chip 1  (serial: kraken_ch1)  →  dmr_monitor instance 2
  ├── chip 2  (serial: kraken_ch2)  →  dmr_monitor instance 3
  ├── chip 3  (serial: kraken_ch3)  →  dmr_monitor instance 4
  └── chip 4  (serial: kraken_ch4)  →  dmr_monitor instance 5

NooElec #1 (serial: inmarsat_1)  →  Inmarsat decoder (JAERO or similar)
NooElec #2 (serial: inmarsat_2)  →  Inmarsat decoder (JAERO or similar)
```

Three USB cables total. Seven independent processes. All coexist without conflict.

### Resource usage at full deployment

| Resource | Used | Available | Headroom |
|----------|------|-----------|---------|
| CPU | ~20% of one core | 4 cores | ~380% remaining |
| RAM | ~140 MB | 16 GB | 15.8 GB remaining |
| USB bandwidth | ~3.5 MB/s | ~400 MB/s | <1% |

No resource is under any meaningful pressure.

### Step 1 — Set unique serials on all seven chips

Before setting serials, stop any KrakenSDR software that may be holding the devices. With all devices connected and no software running:

```bash
rtl_test -t
```

Note which index corresponds to which physical device. Then set serials:

```bash
# KrakenSDR chips (indices 0-4 — verify with rtl_test first)
rtl_eeprom -d 0 -s kraken_ch0
rtl_eeprom -d 1 -s kraken_ch1
rtl_eeprom -d 2 -s kraken_ch2
rtl_eeprom -d 3 -s kraken_ch3
rtl_eeprom -d 4 -s kraken_ch4

# NooElec dongles (indices 5-6 — verify with rtl_test first)
rtl_eeprom -d 5 -s inmarsat_1
rtl_eeprom -d 6 -s inmarsat_2
```

After each `rtl_eeprom` write you will be prompted to confirm. Unplug and replug all devices after writing all serials. Verify:

```bash
rtl_test -t
```

All seven devices should now show distinct serial numbers regardless of enumeration order.

### Step 2 — Build seven binaries

Each instance needs its own binary with its serial and channel configuration compiled in. Create a separate build directory for each:

```bash
mkdir -p ~/dmr_monitor/builds/{kraken_ch0,kraken_ch1,kraken_ch2,kraken_ch3,kraken_ch4,inmarsat_1,inmarsat_2}
```

For each KrakenSDR instance, edit `src/main.cpp` before building — set the serial and the appropriate centre frequency and channel table for that instance. Example for `kraken_ch0`:

```cpp
cap_cfg.serial      = "kraken_ch0";
cap_cfg.centre_freq = 446099000;   // adjust per instance if covering different bands
```

Build each instance:

```bash
for INSTANCE in kraken_ch0 kraken_ch1 kraken_ch2 kraken_ch3 kraken_ch4; do
    cd ~/dmr_monitor/builds/$INSTANCE
    # Edit ../../src/main.cpp: set serial = "$INSTANCE" and desired centre_freq
    cmake ../.. -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$(pwd)
    make -j$(nproc)
    mv dmr_monitor dmr_monitor_$INSTANCE
done
```

Repeat for `inmarsat_1` and `inmarsat_2` if those also use `dmr_monitor` (or substitute your Inmarsat decoder binary there).

Copy all binaries to a common location:

```bash
sudo mkdir -p /usr/local/bin/dmr_monitor
sudo cp ~/dmr_monitor/builds/kraken_ch*/dmr_monitor_kraken_ch* /usr/local/bin/dmr_monitor/
sudo cp ~/dmr_monitor/builds/inmarsat_*/dmr_monitor_inmarsat_* /usr/local/bin/dmr_monitor/
```

### Step 3 — Create the systemd template unit

A single template unit file handles all seven instances. Create `/etc/systemd/system/dmr_monitor@.service`:

```ini
[Unit]
Description=DMR monitor — instance %i
After=network.target
# Ensure all instances restart together if the system comes back from suspend
After=suspend.target

[Service]
Type=simple
User=YOUR_USERNAME
ExecStart=/usr/local/bin/dmr_monitor/dmr_monitor_%i
StandardOutput=append:/var/log/dmr_monitor/%i.jsonl
StandardError=append:/var/log/dmr_monitor/%i.log
Restart=on-failure
RestartSec=5
# Give the RTL-SDR USB devices time to enumerate after boot
ExecStartPre=/bin/sleep 3

[Install]
WantedBy=multi-user.target
```

The `%i` specifier is replaced by the instance name when the service is started. `ExecStartPre=/bin/sleep 3` gives USB devices time to fully enumerate before `librtlsdr` tries to open them — important at boot when all seven devices appear simultaneously.

### Step 4 — Create the log directory

```bash
sudo mkdir -p /var/log/dmr_monitor
sudo chown YOUR_USERNAME:YOUR_USERNAME /var/log/dmr_monitor
```

### Step 5 — Enable and start all seven services

```bash
sudo systemctl daemon-reload

# Enable all instances (start automatically at boot)
for INSTANCE in kraken_ch0 kraken_ch1 kraken_ch2 kraken_ch3 kraken_ch4 inmarsat_1 inmarsat_2; do
    sudo systemctl enable dmr_monitor@$INSTANCE
done

# Start all instances now
sudo systemctl start dmr_monitor@{kraken_ch0,kraken_ch1,kraken_ch2,kraken_ch3,kraken_ch4,inmarsat_1,inmarsat_2}
```

### Step 6 — Verify all seven are running

```bash
systemctl status 'dmr_monitor@*'
```

Expected output for each instance:

```
● dmr_monitor@kraken_ch0.service - DMR monitor — instance kraken_ch0
     Loaded: loaded (/etc/systemd/system/dmr_monitor@.service; enabled)
     Active: active (running) since ...
```

### Managing the services

| Task | Command |
|------|---------|
| Status of all instances | `systemctl status 'dmr_monitor@*'` |
| Stop one instance | `sudo systemctl stop dmr_monitor@kraken_ch0` |
| Restart one instance | `sudo systemctl restart dmr_monitor@kraken_ch2` |
| Stop all instances | `sudo systemctl stop 'dmr_monitor@*'` |
| Restart all instances | `sudo systemctl restart 'dmr_monitor@*'` |
| Disable autostart for one | `sudo systemctl disable dmr_monitor@inmarsat_1` |
| View live log for one instance | `journalctl -fu dmr_monitor@kraken_ch0` |
| View all systemd output | `journalctl -fu 'dmr_monitor@*'` |

### Monitoring the JSON output streams

Watch all five DMR event streams simultaneously:

```bash
tail -f /var/log/dmr_monitor/kraken_ch*.jsonl
```

Watch all seven streams:

```bash
tail -f /var/log/dmr_monitor/*.jsonl
```

Filter only GPS events across all instances:

```bash
tail -f /var/log/dmr_monitor/*.jsonl | grep '"type":"gps"'
```

Filter events from a specific source radio ID across all instances:

```bash
tail -f /var/log/dmr_monitor/*.jsonl | grep '"src":1234567'
```

### Log rotation

Seven continuously running services will accumulate JSONL log files. Set up logrotate to prevent disk fill:

Create `/etc/logrotate.d/dmr_monitor`:

```
/var/log/dmr_monitor/*.jsonl {
    daily
    rotate 30
    compress
    delaycompress
    missingok
    notifempty
    copytruncate
}

/var/log/dmr_monitor/*.log {
    daily
    rotate 7
    compress
    delaycompress
    missingok
    notifempty
    copytruncate
}
```

`copytruncate` is used instead of `create` because the services have the log files open continuously — truncating in place avoids the need to restart the services after rotation.

### Node-RED aggregation

Each of the five DMR instances sends JSON datagrams to UDP port 41414 by default. If all five send to the same port on the same host, Node-RED receives all events through a single **udp in** node — the `ch` field in each JSON object identifies which instance produced the event.

If you want to separate them by instance in Node-RED, assign a different UDP port to each binary:

```cpp
// kraken_ch0/main.cpp
static constexpr uint16_t UDP_PORT = 41414;

// kraken_ch1/main.cpp
static constexpr uint16_t UDP_PORT = 41415;

// etc.
```

Then use five separate **udp in** nodes in Node-RED, one per port, and route their outputs independently. Which approach is cleaner depends on your flow — a single stream with `ch`-based switching is usually simpler.

---

*Document version: 1.2 — 2026-04-08*
