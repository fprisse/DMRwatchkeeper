#include "capture.h"
#include "demod_chain.h"
#include "ringbuffer.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

/*
 * dmr_monitor — Phase 2
 *
 * Adds the complete demodulation pipeline:
 *   FM discriminator -> symbol timing recovery -> 4FSK slicer
 *
 * A single "channelizer thread" reads raw IQ from the ring buffer,
 * passes each block through every configured channel's DemodChain,
 * and receives dibits via callback.
 *
 * Phase 3 will attach burst sync detectors to those dibit callbacks.
 * For now, dibits are counted and statistics printed per second.
 *
 * Channel configuration: edit CHANNELS[] below to match your target band.
 * Frequencies are channel centres in Hz on the 12.5 kHz PMR446 raster.
 */

// Signal handling
static std::atomic<bool> g_running{true};
static void signal_handler(int) {
    g_running.store(false, std::memory_order_release);
}

// Channel table
struct ChannelDef {
    float       freq_hz;
    const char* label;
};

static const ChannelDef CHANNELS[] = {
    { 446006250.0f, "446.006" },
    { 446018750.0f, "446.019" },
    { 446031250.0f, "446.031" },
    { 446043750.0f, "446.044" },
    { 446056250.0f, "446.056" },
    { 446068750.0f, "446.069" },
    { 446081250.0f, "446.081" },
    { 446093750.0f, "446.094" },
    { 446106250.0f, "446.106" },
    { 446118750.0f, "446.119" },
    { 446131250.0f, "446.131" },
    { 446143750.0f, "446.144" },
    { 446156250.0f, "446.156" },
    { 446168750.0f, "446.169" },
    { 446181250.0f, "446.181" },
    { 446193750.0f, "446.194" },
};
static constexpr int NUM_CHANNELS = (int)(sizeof(CHANNELS) / sizeof(CHANNELS[0]));

// Dibit counters per channel (Phase 2 debug only)
static std::atomic<uint64_t> g_dibit_counts[NUM_CHANNELS];

// Channelizer thread: reads ring buffer, feeds all DemodChains
static void channelizer_thread(Capture* cap,
                                std::vector<std::unique_ptr<DemodChain>>* chains)
{
    constexpr size_t BLOCK_PAIRS = 2048;
    constexpr size_t BLOCK_BYTES = BLOCK_PAIRS * 2;
    std::vector<uint8_t> block(BLOCK_BYTES);

    fprintf(stderr, "[channelizer] started — %d channels\n", NUM_CHANNELS);

    while (g_running.load(std::memory_order_relaxed)) {
        while (cap->ring().available() < BLOCK_BYTES) {
            if (!g_running.load(std::memory_order_relaxed)) return;
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        const size_t got = cap->ring().read(block.data(), BLOCK_BYTES);
        if (got < BLOCK_BYTES) continue;

        for (auto& chain : *chains) {
            chain->process(block.data(), BLOCK_PAIRS);
        }
    }
    fprintf(stderr, "[channelizer] stopped\n");
}

int main(int, char*[]) {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Capture config
    CaptureConfig cap_cfg;
    cap_cfg.device_index  = 0;
    cap_cfg.centre_freq   = 446099000;
    cap_cfg.sample_rate   = 250000;
    cap_cfg.gain_db       = 30;
    cap_cfg.auto_gain     = false;
    cap_cfg.ring_capacity = 4 * 1024 * 1024;

    // Build one DemodChain per channel
    std::vector<std::unique_ptr<DemodChain>> chains;
    chains.reserve(NUM_CHANNELS);

    for (int i = 0; i < NUM_CHANNELS; ++i) {
        DemodChain::Config dc;
        dc.channel_freq_hz = CHANNELS[i].freq_hz;
        dc.centre_freq_hz  = (float)cap_cfg.centre_freq;
        dc.channel_idx     = i;
        dc.label           = CHANNELS[i].label;

        // Phase 3 will replace this lambda with burst sync detector
        auto cb = [i](uint8_t /*dibit*/, int /*ch*/) {
            g_dibit_counts[i].fetch_add(1, std::memory_order_relaxed);
        };

        chains.push_back(std::make_unique<DemodChain>(dc, std::move(cb)));
    }

    // Start capture
    Capture cap(cap_cfg);
    try {
        cap.start();
    } catch (const std::exception& e) {
        fprintf(stderr, "[main] fatal: %s\n", e.what());
        return EXIT_FAILURE;
    }

    // Launch channelizer thread
    std::thread chann_thread(channelizer_thread, &cap, &chains);

    // Status loop
    fprintf(stderr, "\n[dmr_monitor] Phase 2 running — Ctrl-C to stop\n\n");
    fprintf(stderr, "%-10s  %-10s  %-8s  %-10s  %s\n",
            "Channel", "Sym/s", "omega", "outer_lvl", "Total");

    std::vector<uint64_t> prev_syms(NUM_CHANNELS, 0);

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        for (int i = 0; i < NUM_CHANNELS; ++i) {
            const uint64_t total = chains[i]->symbols_recovered();
            const uint64_t delta = total - prev_syms[i];
            prev_syms[i] = total;

            fprintf(stderr, "%-10s  %-10llu  %-8.3f  %-10.4f  %llu\n",
                    chains[i]->label().c_str(),
                    (unsigned long long)delta,
                    (double)chains[i]->timing_omega(),
                    (double)chains[i]->slicer_outer(),
                    (unsigned long long)total);
        }
        fprintf(stderr, "  RTL: %.1f MB  overruns: %llu\n\n",
                cap.bytes_captured() / 1e6,
                (unsigned long long)cap.ring().overrun_count());
    }

    fprintf(stderr, "\n[main] shutting down...\n");
    if (chann_thread.joinable()) chann_thread.join();
    cap.stop();
    return EXIT_SUCCESS;
}
