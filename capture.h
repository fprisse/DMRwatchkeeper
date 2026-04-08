#pragma once

#include "ringbuffer.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

/*
 * CaptureConfig
 *
 * All hardware parameters for one RTL-SDR device.
 * Centre frequency is set to the midpoint of your channel band.
 * For 446.002–446.196 MHz this is 446.099 MHz (~446.100 MHz).
 *
 * Sample rate: 250 000 S/s gives 250 kHz bandwidth — enough to cover all
 * 16 possible 12.5 kHz DMR channels in that span with clean margin.
 *
 * The channelizer (Phase 2) will split this wideband stream into per-channel
 * baseband streams at 12.5 kHz each.
 */
struct CaptureConfig {
    uint32_t    device_index  = 0;              // RTL-SDR USB device index
    uint32_t    centre_freq   = 446'099'000;    // Hz  (446.099 MHz)
    uint32_t    sample_rate   = 250'000;        // S/s (250 kSPS)
    int         gain_db       = 30;             // tenths of dB in librtlsdr
                                                // 30 = 30 dB; 0 = auto-gain
    bool        auto_gain     = false;
    size_t      ring_capacity = 4 * 1024 * 1024; // 4 MB ring buffer
};

/*
 * Capture
 *
 * Owns an RTL-SDR device and a background thread that feeds raw IQ bytes
 * into a RingBuffer.
 *
 * Usage:
 *   CaptureConfig cfg;
 *   cfg.centre_freq = 446'099'000;
 *   Capture cap(cfg);
 *   cap.start();
 *   // consumer reads from cap.ring()
 *   cap.stop();
 */
class Capture {
public:
    explicit Capture(const CaptureConfig& cfg);
    ~Capture();

    Capture(const Capture&)            = delete;
    Capture& operator=(const Capture&) = delete;

    // Open device, configure, and launch the reader thread.
    // Throws std::runtime_error on failure.
    void start();

    // Signal stop, wait for the thread, close device.
    void stop();

    // Access the ring buffer for downstream reading.
    RingBuffer& ring() { return ring_; }

    // True while the capture thread is running.
    bool running() const { return running_.load(std::memory_order_relaxed); }

    // Diagnostic: bytes captured since start()
    uint64_t bytes_captured() const {
        return bytes_captured_.load(std::memory_order_relaxed);
    }

private:
    // Static trampoline for librtlsdr async callback
    static void rtlsdr_callback(unsigned char* buf, uint32_t len, void* ctx);

    // Instance-level handler called from the trampoline
    void on_samples(unsigned char* buf, uint32_t len);

    // Thread entry: calls rtlsdr_read_async, blocks until cancel
    void reader_thread();

    CaptureConfig           cfg_;
    RingBuffer              ring_;
    void*                   dev_{nullptr};   // rtlsdr_dev_t* — void* to avoid
                                             // dragging rtl-sdr.h into this header

    std::thread             thread_;
    std::atomic<bool>       running_{false};
    std::atomic<uint64_t>   bytes_captured_{0};
};
