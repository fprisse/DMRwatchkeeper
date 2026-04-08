#pragma once

#include <array>
#include <cstdint>
#include <functional>

/*
 * BurstSync
 *
 * Scans the raw dibit stream for DMR sync words, assembles complete bursts,
 * and delivers them via callback to the Layer 2 parser (Phase 4).
 *
 * ── DMR burst structure (ETSI TS 102 361-1 §9.1) ────────────────────────────
 *
 *  [First half: 49 dibits][SYNC: 24 dibits][Second half: 49 dibits][Guard: 10 dibits]
 *   = 98 + 48 + 98 + 20 = 264 bits total per timeslot burst
 *
 * The 48-bit sync word identifies burst type and sits at a fixed position.
 * We detect it via a sliding 48-bit shift register compared against the
 * four known sync patterns (Hamming distance threshold ≤ 4 bits).
 *
 * On sync match:
 *   1. Recover 49 first-half dibits from the history buffer
 *   2. Record burst type from matched sync pattern
 *   3. Collect 49 more dibits (second half)
 *   4. Assemble Burst struct → invoke callback
 *
 * ── Sync patterns (ETSI TS 102 361-1 Table 9.11) ────────────────────────────
 *
 *   BS_VOICE  0x755FD7DF75F7   Base station → mobile, voice traffic
 *   MS_VOICE  0xDFF57D755FF7   Mobile → base station, voice traffic
 *   BS_DATA   0xD5D7F77FD757   Base station → mobile, data traffic
 *   MS_DATA   0x7F7D55F5DF5D   Mobile → base station, data traffic
 *
 * ── Timeslot assignment ──────────────────────────────────────────────────────
 *
 * Proper timeslot demultiplexing requires CACH parsing or a BS frame timing
 * reference, neither of which is available in a passive receiver without
 * CACH decoding (a Phase 4+ task).
 *
 * For Phase 3 we assign timeslots by simple alternation: odd bursts → TS1,
 * even bursts → TS2.  This is correct approximately 50% of the time without
 * frame sync.  Add a comment in Phase 4 when CACH is decoded and can correct this.
 */

class BurstSync {
public:
    enum class BurstType : uint8_t {
        BS_VOICE,
        MS_VOICE,
        BS_DATA,
        MS_DATA,
        UNKNOWN
    };

    struct Burst {
        BurstType type      = BurstType::UNKNOWN;
        int       timeslot  = 1;      // 1 or 2 (alternating estimate)
        int       channel   = 0;      // channel index, passed through
        uint64_t  sync_bits = 0;      // the matched 48-bit sync word

        // 98 payload dibits: [0..48] = first half, [49..97] = second half
        // Sync dibits are NOT included — burst type already known from sync_bits.
        // Phase 4 BPTC decoder consumes these 98 dibits to recover 96 bits of LC.
        uint8_t dibits[98]{};
    };

    using BurstCallback = std::function<void(const Burst&)>;

    explicit BurstSync(BurstCallback cb, int channel_idx = 0);

    /*
     * push_dibit()
     * Feed one dibit (value 0–3) from the 4FSK slicer.
     * Callback is invoked whenever a complete burst is assembled.
     */
    void push_dibit(uint8_t dibit);

    // Diagnostics
    uint64_t bursts_detected()  const { return burst_count_; }
    uint64_t sync_matches()     const { return sync_matches_; }

    static const char* burst_type_str(BurstType t);

private:
    // ── Sync pattern constants ─────────────────────────────────────────────
    // Source: ETSI TS 102 361-1 Table 9.11
    // Stored as 48-bit values in the lower 48 bits of uint64_t
    static constexpr uint64_t kMask48     = 0x0000FFFFFFFFFFFFULL;
    static constexpr uint64_t kSyncBsVoice = 0x755FD7DF75F7ULL;
    static constexpr uint64_t kSyncMsVoice = 0xDFF57D755FF7ULL;
    static constexpr uint64_t kSyncBsData  = 0xD5D7F77FD757ULL;
    static constexpr uint64_t kSyncMsData  = 0x7F7D55F5DF5DULL;
    static constexpr int      kMaxErrors   = 4;  // max Hamming distance for match

    // ── Sliding shift register ─────────────────────────────────────────────
    uint64_t sync_reg_ = 0;   // lower 48 bits are the last 24 received dibits

    // ── History buffer ─────────────────────────────────────────────────────
    // Must hold at least 73 dibits (49 first-half + 24 sync) before a match.
    static constexpr int kHistLen = 128;
    uint8_t hist_[kHistLen]{};
    int     hist_head_ = 0;   // next write index

    // ── Collection state ───────────────────────────────────────────────────
    enum class State { HUNTING, COLLECTING };
    State   state_        = State::HUNTING;
    int     collect_idx_  = 0;    // how many second-half dibits collected so far
    Burst   current_{};

    // ── Counters ───────────────────────────────────────────────────────────
    int      ts_toggle_    = 0;
    uint64_t burst_count_  = 0;
    uint64_t sync_matches_ = 0;
    int      channel_idx_  = 0;

    BurstCallback cb_;

    // ── Helpers ────────────────────────────────────────────────────────────
    static int hamming48(uint64_t a, uint64_t b);
    BurstType  match_sync(uint64_t word) const;
    void       extract_first_half(uint8_t* out) const;
};
