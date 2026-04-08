#include "capture.h"
#include "demod_chain.h"
#include "slot_manager.h"
#include "burst_sync.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <thread>
#include <vector>

/*
 * dmr_monitor -- Phase 3
 *
 * Adds burst sync detection and timeslot state machine.
 * Every assembled burst is printed to stderr with:
 *   timestamp, channel, timeslot, burst type, slot state, burst count
 *
 * Phase 4 will attach BPTC decoder + LC parser to the Layer2 callback,
 * replacing the fprintf below with Source ID, Dest ID, and GPS output.
 */

static std::atomic<bool> g_running{true};
static void signal_handler(int) {
    g_running.store(false, std::memory_order_release); }

struct ChannelDef { float freq_hz; const char* label; };

static const ChannelDef CHANNELS[] = {
    { 446006250.0f, "446.006" }, { 446018750.0f, "446.019" },
    { 446031250.0f, "446.031" }, { 446043750.0f, "446.044" },
    { 446056250.0f, "446.056" }, { 446068750.0f, "446.069" },
    { 446081250.0f, "446.081" }, { 446093750.0f, "446.094" },
    { 446106250.0f, "446.106" }, { 446118750.0f, "446.119" },
    { 446131250.0f, "446.131" }, { 446143750.0f, "446.144" },
    { 446156250.0f, "446.156" }, { 446168750.0f, "446.169" },
    { 446181250.0f, "446.181" }, { 446193750.0f, "446.194" },
};
static constexpr int NUM_CHANNELS = (int)(sizeof(CHANNELS) / sizeof(CHANNELS[0]));

// ISO-8601 timestamp into buf (at least 26 bytes)
static void timestamp(char* buf) {
    time_t t = time(nullptr);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, 26, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void channelizer_thread(Capture* cap,
                                std::vector<std::unique_ptr<DemodChain>>* chains)
{
    constexpr size_t BLOCK_PAIRS = 2048;
    constexpr size_t BLOCK_BYTES = BLOCK_PAIRS * 2;
    std::vector<uint8_t> block(BLOCK_BYTES);

    while (g_running.load(std::memory_order_relaxed)) {
        while (cap->ring().available() < BLOCK_BYTES) {
            if (!g_running.load(std::memory_order_relaxed)) return;
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        const size_t got = cap->ring().read(block.data(), BLOCK_BYTES);
        if (got < BLOCK_BYTES) continue;
        for (auto& chain : *chains)
            chain->process(block.data(), BLOCK_PAIRS);
    }
}

int main(int, char*[]) {
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    CaptureConfig cap_cfg;
    cap_cfg.device_index  = 0;
    cap_cfg.centre_freq   = 446099000;
    cap_cfg.sample_rate   = 250000;
    cap_cfg.gain_db       = 30;
    cap_cfg.auto_gain     = false;
    cap_cfg.ring_capacity = 4 * 1024 * 1024;

    std::vector<std::unique_ptr<DemodChain>> chains;
    chains.reserve(NUM_CHANNELS);

    for (int i = 0; i < NUM_CHANNELS; ++i) {
        DemodChain::Config dc;
        dc.channel_freq_hz = CHANNELS[i].freq_hz;
        dc.centre_freq_hz  = (float)cap_cfg.centre_freq;
        dc.channel_idx     = i;
        dc.label           = CHANNELS[i].label;

        // Phase 4 will replace this lambda with BPTC + LC + GPS parser
        const char* lbl = CHANNELS[i].label;
        auto cb = [lbl](const SlotManager::SlotEvent& ev) {
            char ts[26];
            timestamp(ts);
            fprintf(stdout,
                    "%s  ch=%-8s  ts=%d  burst=%-9s  state=%-6s  n=%llu\n",
                    ts,
                    lbl,
                    ev.burst.timeslot,
                    BurstSync::burst_type_str(ev.burst.type),
                    SlotManager::state_str(ev.state),
                    (unsigned long long)ev.burst_count);
            fflush(stdout);
        };

        chains.push_back(std::make_unique<DemodChain>(dc, std::move(cb)));
    }

    Capture cap(cap_cfg);
    try { cap.start(); }
    catch (const std::exception& e) {
        fprintf(stderr, "[main] fatal: %s\n", e.what());
        return EXIT_FAILURE;
    }

    std::thread chann_thread(channelizer_thread, &cap, &chains);

    fprintf(stderr, "[dmr_monitor] Phase 3 running -- Ctrl-C to stop\n");
    fprintf(stderr, "Burst events printed to stdout.\n\n");

    // Status loop: print per-channel burst counts every 5 seconds
    int tick = 0;
    std::vector<uint64_t> prev_bursts(NUM_CHANNELS, 0);

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (++tick < 5) continue;
        tick = 0;

        fprintf(stderr, "\n-- 5s burst summary --\n");
        fprintf(stderr, "%-10s  %-8s  %-8s  %-8s\n",
                "Channel", "Bursts", "TS1", "TS2");
        for (int i = 0; i < NUM_CHANNELS; ++i) {
            const uint64_t b = chains[i]->bursts_detected();
            const uint64_t d = b - prev_bursts[i];
            prev_bursts[i] = b;
            if (d > 0) {
                fprintf(stderr, "%-10s  %-8llu  %-8s  %-8s\n",
                        chains[i]->label().c_str(),
                        (unsigned long long)d,
                        SlotManager::state_str(chains[i]->slot_state(1)),
                        SlotManager::state_str(chains[i]->slot_state(2)));
            }
        }
        fprintf(stderr, "Total RTL: %.1f MB  overruns: %llu\n",
                cap.bytes_captured() / 1e6,
                (unsigned long long)cap.ring().overrun_count());
    }

    fprintf(stderr, "\n[main] shutting down...\n");
    if (chann_thread.joinable()) chann_thread.join();
    cap.stop();
    return EXIT_SUCCESS;
}
