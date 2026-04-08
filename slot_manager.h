#pragma once

#include "burst_sync.h"

#include <cstdint>
#include <functional>

/*
 * SlotManager
 *
 * Receives bursts from BurstSync and maintains a state machine for each
 * of the two DMR timeslots (TS1, TS2).
 *
 * ── Per-timeslot state machine ────────────────────────────────────────────────
 *
 *  IDLE ──(voice header burst)──► VOICE
 *  VOICE ──(voice frame)────────► VOICE
 *  VOICE ──(terminator burst)───► IDLE
 *  IDLE ──(data header burst)───► DATA
 *  DATA ──(data block)──────────► DATA
 *  DATA ──(data terminator)─────► IDLE
 *  Any ──(timeout: 4+ missed)───► IDLE
 *
 * For Phase 3 the state machine is lightweight — it tracks state and
 * forwards all bursts to the Layer 2 callback unchanged.
 *
 * Phase 4 attaches the BPTC decoder + LC parser to that callback.
 *
 * ── Output ────────────────────────────────────────────────────────────────────
 *
 * SlotManager emits a SlotEvent for every received burst, including:
 *   - The raw Burst (98 payload dibits)
 *   - The current slot state (IDLE / VOICE / DATA)
 *   - Running burst counters per slot
 *
 * In Phase 4 the Layer2Callback will be the BPTC + LC + GPS parser.
 */

class SlotManager {
public:
    enum class SlotState : uint8_t {
        IDLE,
        VOICE,
        DATA
    };

    struct SlotEvent {
        BurstSync::Burst  burst;       // 98 payload dibits + metadata
        SlotState         state;       // state AFTER processing this burst
        uint64_t          burst_count; // total bursts seen on this timeslot
    };

    // Layer 2 callback — Phase 4 replaces the lambda body with BPTC decoder
    using Layer2Callback = std::function<void(const SlotEvent&)>;

    explicit SlotManager(Layer2Callback cb, int channel_idx = 0);

    /*
     * on_burst()
     * Called by BurstSync for each assembled burst.
     * Routes to the correct timeslot state machine and invokes Layer2Callback.
     */
    void on_burst(const BurstSync::Burst& burst);

    // Diagnostics
    SlotState   slot_state(int ts) const;
    uint64_t    slot_bursts(int ts) const;

    static const char* state_str(SlotState s);

private:
    struct SlotCtx {
        SlotState state      = SlotState::IDLE;
        uint64_t  count      = 0;
        int       miss_count = 0;   // consecutive missed bursts (timeout)
    };

    static constexpr int kMissTimeout = 4;  // bursts before IDLE timeout

    SlotCtx       slots_[2];  // index 0 = TS1, index 1 = TS2
    Layer2Callback cb_;
    int           channel_idx_;

    void advance_state(SlotCtx& ctx, BurstSync::BurstType type);
};
