# dmr_monitor

DMR metadata extraction pipeline — Group/User IDs and GPS from RTL-SDR.

## Target band
446.002 – 446.196 MHz (194 kHz span, up to 16 × 12.5 kHz DMR channels)

## Build

### Dependencies (Ubuntu 20+)
```bash
sudo apt install build-essential cmake librtlsdr-dev
```

### Compile
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Run
```bash
./dmr_monitor
```
Ctrl-C for clean shutdown.

## Phase roadmap

| Phase | Status | Description |
|-------|--------|-------------|
| 1 | ✅ current | RTL-SDR capture, ring buffer, status loop |
| 2 | pending | FM discriminator, symbol timing, 4FSK slicer |
| 3 | pending | DMR burst sync, timeslot demux, state machine |
| 4 | pending | BPTC(196,96) FEC, LC parser, GPS/LRRP parser |
| 5 | pending | JSON stdout output |

## Architecture

```
RTL-SDR (446.099 MHz, 250 kSPS)
    │
    ▼ [librtlsdr async callback]
RingBuffer (4 MB, lock-free SPSC)
    │
    ▼ [Phase 2: channelizer thread]
Per-channel IQ streams (12.5 kHz each)
    │
    ▼ [Phase 2: demod threads, one per channel]
FM discriminator → symbol timing → 4FSK slicer
    │
    ▼ [Phase 3]
Burst sync → timeslot demux → state machine
    │
    ▼ [Phase 4]
BPTC FEC → LC parser → GPS parser
    │
    ▼ [Phase 5]
JSON stdout
```

## Notes

- Gain: default 30 dB manual. Adjust `cfg.gain_db` in main.cpp for your
  antenna and local RF environment. If you see overruns, reduce gain.
- Ring buffer: 4 MB = ~2 seconds of IQ at 250 kSPS. More than sufficient.
- The drain loop in Phase 1 main() will be replaced by real demod threads
  in Phase 2.
