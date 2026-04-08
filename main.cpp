#include "bptc19696.h"
#include "burst_sync.h"
#include "capture.h"
#include "demod_chain.h"
#include "lc_parser.h"
#include "slot_manager.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

/*
 * dmr_monitor -- Phase 4
 *
 * Adds BPTC(196,96) decoding, LC parsing, and GPS extraction.
 *
 * Output (JSON Lines to stdout, one object per event):
 *
 * Voice call:
 *   {"ts":"...","ch":"446.006","slot":1,"type":"voice",
 *    "src":1234567,"dst":9001,"group":true}
 *
 * GPS report:
 *   {"ts":"...","ch":"446.006","slot":2,"type":"gps",
 *    "src":1234567,"lat":52.3731,"lon":4.8922}
 *
 * Unknown / undecoded burst:
 *   {"ts":"...","ch":"446.006","slot":1,"type":"burst",
 *    "burst_type":"BS_DATA","state":"DATA"}
 */

static std::atomic<bool> g_running{true};
static void signal_handler(int) {
    g_running.store(false, std::memory_order_release);
}

// Mutex to serialise stdout writes from all channel callbacks
static std::mutex g_stdout_mtx;

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

static void iso8601(char* buf26) {
    time_t t = time(nullptr);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf26, 26, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

// ── Layer 2 callback: runs for every assembled burst ─────────────────────────
// One instance per channel (captures channel label by value).

static SlotManager::Layer2Callback make_layer2_cb(const char* ch_label) {

    return [ch_label](const SlotManager::SlotEvent& ev) {

        const BurstSync::Burst& burst = ev.burst;

        // Decode BPTC
        BPTC19696 bptc;
        uint8_t lc_bits[96];
        const bool bptc_ok = bptc.decode(burst.dibits, lc_bits);

        if (!bptc_ok) {
            // Uncorrectable error — skip this burst silently.
            // Increase verbosity here if you want to log FEC failures.
            return;
        }

        char ts[26];
        iso8601(ts);

        using BT = BurstSync::BurstType;

        // ── Voice burst → parse LC for Source/Dest IDs ────────────────────
        if (burst.type == BT::BS_VOICE || burst.type == BT::MS_VOICE) {
            const LcResult lc = LcParser::parse_voice_lc(lc_bits);
            if (!lc.valid) return;

            std::lock_guard<std::mutex> lk(g_stdout_mtx);
            printf("{\"ts\":\"%s\",\"ch\":\"%s\",\"slot\":%d,"
                   "\"type\":\"voice\","
                   "\"src\":%u,\"dst\":%u,\"group\":%s}\n",
                   ts, ch_label, burst.timeslot,
                   lc.src_id, lc.dst_id,
                   lc.group ? "true" : "false");
            fflush(stdout);
            return;
        }

        // ── Data burst → attempt GPS parse ────────────────────────────────
        if (burst.type == BT::BS_DATA || burst.type == BT::MS_DATA) {
            const GpsResult gps = LcParser::parse_gps(lc_bits);
            if (gps.valid) {
                std::lock_guard<std::mutex> lk(g_stdout_mtx);
                // For data bursts we don't have a Source ID at this layer
                // without a full data header decode (Phase 5 extension).
                // We print the coordinates and mark src as 0.
                printf("{\"ts\":\"%s\",\"ch\":\"%s\",\"slot\":%d,"
                       "\"type\":\"gps\","
                       "\"src\":0,\"lat\":%.6f,\"lon\":%.6f}\n",
                       ts, ch_label, burst.timeslot,
                       gps.lat, gps.lon);
                fflush(stdout);
                return;
            }
            // Non-GPS data burst: fall through to generic output below
        }

        // ── Generic burst event (no decoded payload) ───────────────────────
        // Uncomment if you want to log every burst, not just decoded ones.
        /*
        {
            std::lock_guard<std::mutex> lk(g_stdout_mtx);
            printf("{\"ts\":\"%s\",\"ch\":\"%s\",\"slot\":%d,"
                   "\"type\":\"burst\","
                   "\"burst_type\":\"%s\",\"state\":\"%s\"}\n",
                   ts, ch_label, burst.timeslot,
                   BurstSync::burst_type_str(burst.type),
                   SlotManager::state_str(ev.state));
            fflush(stdout);
        }
        */
    };
}

// ── Channelizer thread ────────────────────────────────────────────────────────

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

// ── main ─────────────────────────────────────────────────────────────────────

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

        chains.push_back(std::make_unique<DemodChain>(
            dc, make_layer2_cb(CHANNELS[i].label)));
    }

    Capture cap(cap_cfg);
    try { cap.start(); }
    catch (const std::exception& e) {
        fprintf(stderr, "[main] fatal: %s\n", e.what());
        return EXIT_FAILURE;
    }

    std::thread chann_thread(channelizer_thread, &cap, &chains);

    fprintf(stderr, "[dmr_monitor] Phase 4 running -- JSON to stdout, Ctrl-C to stop\n\n");

    // Status: print burst/symbol counts every 10 seconds to stderr
    int tick = 0;
    std::vector<uint64_t> prev_bursts(NUM_CHANNELS, 0);

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (++tick < 10) continue;
        tick = 0;

        fprintf(stderr, "-- 10s summary --\n");
        for (int i = 0; i < NUM_CHANNELS; ++i) {
            const uint64_t b = chains[i]->bursts_detected();
            const uint64_t d = b - prev_bursts[i];
            prev_bursts[i] = b;
            if (d > 0) {
                fprintf(stderr, "  %-10s  %llu bursts/10s  ts1=%s  ts2=%s\n",
                        chains[i]->label().c_str(), (unsigned long long)d,
                        SlotManager::state_str(chains[i]->slot_state(1)),
                        SlotManager::state_str(chains[i]->slot_state(2)));
            }
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
