#include "bptc19696.h"
#include "burst_sync.h"
#include "capture.h"
#include "demod_chain.h"
#include "lc_parser.h"
#include "slot_manager.h"
#include "udp_output.h"

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
 * dmr_monitor -- Phase 5
 *
 * Adds UDP output to Node-RED (or any UDP listener).
 *
 * Every JSON event is sent as a single UDP datagram to UDP_HOST:UDP_PORT
 * in addition to being written to stdout.
 *
 * Node-RED setup:
 *   [udp in] port=41414, output="a String"
 *   → [json]
 *   → your flow
 *
 * To change destination, edit UDP_HOST and UDP_PORT below and rebuild.
 */

// ── UDP destination ───────────────────────────────────────────────────────────

static constexpr const char*  UDP_HOST = "127.0.0.1";  // Node-RED host
static constexpr uint16_t     UDP_PORT = 41414;         // Node-RED UDP in port

// ── Globals ───────────────────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};
static void signal_handler(int) {
    g_running.store(false, std::memory_order_release);
}

// Serialises stdout writes and UDP sends from all channel callbacks
static std::mutex    g_output_mtx;
static UdpOutput*    g_udp = nullptr;   // set in main() before threads start

// ── Channel table ─────────────────────────────────────────────────────────────

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

// ── Helpers ───────────────────────────────────────────────────────────────────

static void iso8601(char* buf26) {
    time_t t = time(nullptr);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf26, 26, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

// Emit one JSON event: write to stdout AND send via UDP
static void emit(const char* json) {
    std::lock_guard<std::mutex> lk(g_output_mtx);

    // stdout (for systemd journal / file redirect)
    puts(json);
    fflush(stdout);

    // UDP (for Node-RED)
    if (g_udp && g_udp->is_open()) {
        g_udp->send(json);
    }
}

// ── Layer 2 callback ──────────────────────────────────────────────────────────

static SlotManager::Layer2Callback make_layer2_cb(const char* ch_label) {
    return [ch_label](const SlotManager::SlotEvent& ev) {

        const BurstSync::Burst& burst = ev.burst;

        BPTC19696 bptc;
        uint8_t lc_bits[96];
        if (!bptc.decode(burst.dibits, lc_bits)) return;

        char ts[26];
        iso8601(ts);
        char json[320];

        using BT = BurstSync::BurstType;

        // ── Voice burst ───────────────────────────────────────────────────
        if (burst.type == BT::BS_VOICE || burst.type == BT::MS_VOICE) {
            const LcResult lc = LcParser::parse_voice_lc(lc_bits);
            if (!lc.valid) return;

            snprintf(json, sizeof(json),
                "{\"ts\":\"%s\",\"ch\":\"%s\",\"slot\":%d,"
                "\"type\":\"voice\","
                "\"src\":%u,\"dst\":%u,\"group\":%s}",
                ts, ch_label, burst.timeslot,
                lc.src_id, lc.dst_id,
                lc.group ? "true" : "false");
            emit(json);
            return;
        }

        // ── Data burst — attempt GPS parse ────────────────────────────────
        if (burst.type == BT::BS_DATA || burst.type == BT::MS_DATA) {
            const GpsResult gps = LcParser::parse_gps(lc_bits);
            if (gps.valid) {
                snprintf(json, sizeof(json),
                    "{\"ts\":\"%s\",\"ch\":\"%s\",\"slot\":%d,"
                    "\"type\":\"gps\","
                    "\"src\":0,\"lat\":%.6f,\"lon\":%.6f}",
                    ts, ch_label, burst.timeslot,
                    gps.lat, gps.lon);
                emit(json);
            }
        }

        // Uncomment to emit every burst (including undecoded):
        /*
        snprintf(json, sizeof(json),
            "{\"ts\":\"%s\",\"ch\":\"%s\",\"slot\":%d,"
            "\"type\":\"burst\","
            "\"burst_type\":\"%s\",\"state\":\"%s\"}",
            ts, ch_label, burst.timeslot,
            BurstSync::burst_type_str(burst.type),
            SlotManager::state_str(ev.state));
        emit(json);
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

    // ── UDP output ────────────────────────────────────────────────────────
    UdpOutput udp(UDP_HOST, UDP_PORT);
    g_udp = &udp;
    try {
        udp.open();
    } catch (const std::exception& e) {
        // Non-fatal: log the error and continue without UDP.
        // stdout output still works normally.
        fprintf(stderr, "[main] warning: UDP init failed: %s\n", e.what());
        fprintf(stderr, "[main] continuing with stdout only\n");
    }

    // ── Capture config ────────────────────────────────────────────────────
    CaptureConfig cap_cfg;
    cap_cfg.device_index  = 0;           // fallback if serial not set or not found
    cap_cfg.serial        = "";          // set to e.g. "dmr_monitor" if you have a
                                         // KrakenSDR or multiple dongles — see §18
                                         // of the build manual
    cap_cfg.centre_freq   = 446099000;
    cap_cfg.sample_rate   = 250000;
    cap_cfg.gain_db       = 30;
    cap_cfg.auto_gain     = false;
    cap_cfg.ring_capacity = 4 * 1024 * 1024;

    // ── Build channel demod chains ────────────────────────────────────────
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

    // ── Start capture ─────────────────────────────────────────────────────
    Capture cap(cap_cfg);
    try { cap.start(); }
    catch (const std::exception& e) {
        fprintf(stderr, "[main] fatal: %s\n", e.what());
        udp.close();
        return EXIT_FAILURE;
    }

    std::thread chann_thread(channelizer_thread, &cap, &chains);

    fprintf(stderr,
        "[dmr_monitor] running\n"
        "  JSON  -> stdout\n"
        "  JSON  -> UDP %s:%u\n"
        "  Press Ctrl-C to stop\n\n",
        UDP_HOST, UDP_PORT);

    // ── Status loop ───────────────────────────────────────────────────────
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
        fprintf(stderr, "  RTL: %.1f MB  overruns: %llu  UDP: %s\n\n",
                cap.bytes_captured() / 1e6,
                (unsigned long long)cap.ring().overrun_count(),
                udp.is_open() ? "ok" : "closed");
    }

    fprintf(stderr, "\n[main] shutting down...\n");
    if (chann_thread.joinable()) chann_thread.join();
    cap.stop();
    udp.close();
    return EXIT_SUCCESS;
}
