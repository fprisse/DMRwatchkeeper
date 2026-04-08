#include "capture.h"
#include "ringbuffer.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

/*
 * dmr_monitor — Phase 1
 *
 * At this stage the program:
 *   1. Opens the RTL-SDR dongle
 *   2. Tunes to 446.099 MHz, sets 250 kSPS
 *   3. Fills the ring buffer with raw IQ bytes
 *   4. Prints a status line every second (bytes/s, overruns)
 *
 * Phase 2 will add the FM discriminator and channelizer that consume
 * data from the ring buffer.
 */

// ── Globals for signal handling ──────────────────────────────────────────────

static std::atomic<bool> g_running{true};

static void signal_handler(int /*sig*/) {
    g_running.store(false, std::memory_order_release);
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Install clean shutdown on Ctrl-C / SIGTERM
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── Configuration ────────────────────────────────────────────────────────
    CaptureConfig cfg;
    cfg.device_index = 0;
    cfg.centre_freq  = 446'099'000;   // 446.099 MHz — centre of 446.002–446.196
    cfg.sample_rate  = 250'000;       // 250 kSPS — covers full 194 kHz span
    cfg.gain_db      = 30;            // 30 dB manual gain — adjust for your antenna
    cfg.auto_gain    = false;
    cfg.ring_capacity = 4 * 1024 * 1024;  // 4 MB

    // ── Start capture ─────────────────────────────────────────────────────────
    Capture cap(cfg);

    try {
        cap.start();
    } catch (const std::exception& e) {
        fprintf(stderr, "[main] fatal: %s\n", e.what());
        return EXIT_FAILURE;
    }

    // ── Status loop ───────────────────────────────────────────────────────────
    // TODO Phase 2: Replace this loop with the channelizer + demodulator threads.
    // For now we just drain the ring buffer (discard bytes) and print stats.

    uint64_t last_bytes = 0;

    fprintf(stderr, "\n[dmr_monitor] Phase 1 running — press Ctrl-C to stop\n\n");
    fprintf(stderr, "%-12s %-12s %-12s %-10s\n",
            "Ring avail", "Rate (kB/s)", "Total (MB)", "Overruns");
    fprintf(stderr, "%-12s %-12s %-12s %-10s\n",
            "----------", "----------", "----------", "--------");

    constexpr size_t DRAIN_BUF = 65536;
    static uint8_t drain_buf[DRAIN_BUF];

    while (g_running.load(std::memory_order_relaxed)) {
        // Drain ring buffer (in Phase 2 the demod thread will do this)
        while (cap.ring().available() > DRAIN_BUF) {
            cap.ring().read(drain_buf, DRAIN_BUF);
        }

        // Print stats once per second
        const uint64_t total = cap.bytes_captured();
        const uint64_t delta = total - last_bytes;
        last_bytes = total;

        fprintf(stderr, "%-12zu %-12llu %-12.2f %-10llu\r",
                cap.ring().available(),
                (unsigned long long)(delta / 1000),
                total / 1e6,
                (unsigned long long)cap.ring().overrun_count());
        fflush(stderr);

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    fprintf(stderr, "\n\n[main] shutting down...\n");

    cap.stop();

    return EXIT_SUCCESS;
}
