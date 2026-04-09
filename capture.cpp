#include "capture.h"

#include <rtl-sdr.h>

#include <stdexcept>
#include <cstdio>
#include <cstring>

// ── librtlsdr callback trampoline ────────────────────────────────────────────

void Capture::rtlsdr_callback(unsigned char* buf, uint32_t len, void* ctx) {
    static_cast<Capture*>(ctx)->on_samples(buf, len);
}

void Capture::on_samples(unsigned char* buf, uint32_t len) {
    ring_.write(buf, len);
    bytes_captured_.fetch_add(len, std::memory_order_relaxed);
}

// ── Constructor / Destructor ─────────────────────────────────────────────────

Capture::Capture(const CaptureConfig& cfg)
    : cfg_(cfg)
    , ring_(cfg.ring_capacity)
{}

Capture::~Capture() {
    if (running_) stop();
}

// ── start() ──────────────────────────────────────────────────────────────────

void Capture::start() {
    if (running_) return;

    rtlsdr_dev_t* dev = nullptr;

    // Open device — resolve by serial if configured, otherwise use index
    uint32_t open_index = cfg_.device_index;
    if (!cfg_.serial.empty()) {
        const int idx = rtlsdr_get_index_by_serial(cfg_.serial.c_str());
        if (idx < 0) {
            throw std::runtime_error(
                "Capture: no RTL-SDR device with serial '"
                + cfg_.serial + "' found. "
                + "Check the dongle is connected and the serial was set with: "
                + "rtl_eeprom -d 0 -s " + cfg_.serial);
        }
        open_index = static_cast<uint32_t>(idx);
        fprintf(stderr, "[capture] serial '%s' resolved to device index %u\n",
                cfg_.serial.c_str(), open_index);
    }
    if (rtlsdr_open(&dev, open_index) < 0) {
        throw std::runtime_error("Capture: failed to open RTL-SDR device index "
                                  + std::to_string(open_index));
    }
    dev_ = dev;

    // Sample rate
    if (rtlsdr_set_sample_rate(dev, cfg_.sample_rate) < 0) {
        rtlsdr_close(dev);
        throw std::runtime_error("Capture: failed to set sample rate");
    }

    // Centre frequency
    if (rtlsdr_set_center_freq(dev, cfg_.centre_freq) < 0) {
        rtlsdr_close(dev);
        throw std::runtime_error("Capture: failed to set centre frequency");
    }

    // Gain
    if (cfg_.auto_gain) {
        rtlsdr_set_tuner_gain_mode(dev, 0);  // 0 = auto
    } else {
        rtlsdr_set_tuner_gain_mode(dev, 1);  // 1 = manual
        rtlsdr_set_tuner_gain(dev, cfg_.gain_db * 10);  // gain in tenths of dB
    }

    // Reset buffer before first use
    rtlsdr_reset_buffer(dev);
    ring_.reset();

    running_.store(true, std::memory_order_release);

    // Launch background reader thread
    thread_ = std::thread(&Capture::reader_thread, this);

    fprintf(stderr, "[capture] started — freq=%.3f MHz  SR=%u kSPS  gain=%s\n",
            cfg_.centre_freq / 1e6,
            cfg_.sample_rate / 1000,
            cfg_.auto_gain ? "auto" : std::to_string(cfg_.gain_db).append(" dB").c_str());
}

// ── stop() ───────────────────────────────────────────────────────────────────

void Capture::stop() {
    if (!running_) return;

    running_.store(false, std::memory_order_release);

    // Cancel the async read — this unblocks rtlsdr_read_async in the thread
    if (dev_) {
        rtlsdr_cancel_async(static_cast<rtlsdr_dev_t*>(dev_));
    }

    if (thread_.joinable()) thread_.join();

    if (dev_) {
        rtlsdr_close(static_cast<rtlsdr_dev_t*>(dev_));
        dev_ = nullptr;
    }

    fprintf(stderr, "[capture] stopped — %.1f MB captured  overruns=%llu\n",
            bytes_captured_ / 1e6,
            (unsigned long long)ring_.overrun_count());
}

// ── reader_thread ─────────────────────────────────────────────────────────────

void Capture::reader_thread() {
    /*
     * rtlsdr_read_async blocks until rtlsdr_cancel_async() is called.
     * Each callback delivers a buffer of raw IQ bytes (uint8_t pairs).
     *
     * buf_num  = number of internal USB transfer buffers (default 15 is fine)
     * buf_len  = bytes per transfer buffer
     *            16384 bytes = 8192 IQ pairs = ~33 ms at 250 kSPS
     *            Must be a multiple of 512.
     */
    const uint32_t buf_num = 15;
    const uint32_t buf_len = 16384;

    rtlsdr_read_async(
        static_cast<rtlsdr_dev_t*>(dev_),
        Capture::rtlsdr_callback,
        this,
        buf_num,
        buf_len
    );
    // Returns after rtlsdr_cancel_async()
}
