#include "slot_manager.h"

#include <stdexcept>

// ── Constructor ──────────────────────────────────────────────────────────────

SlotManager::SlotManager(Layer2Callback cb, int channel_idx)
    : cb_(std::move(cb))
    , channel_idx_(channel_idx)
{
    if (!cb_) {
        throw std::invalid_argument("SlotManager: callback must not be null");
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

const char* SlotManager::state_str(SlotState s) {
    switch (s) {
        case SlotState::IDLE:  return "IDLE";
        case SlotState::VOICE: return "VOICE";
        case SlotState::DATA:  return "DATA";
        default:               return "?";
    }
}

SlotManager::SlotState SlotManager::slot_state(int ts) const {
    return slots_[ts == 2 ? 1 : 0].state;
}

uint64_t SlotManager::slot_bursts(int ts) const {
    return slots_[ts == 2 ? 1 : 0].count;
}

// ── State machine transition ──────────────────────────────────────────────────

void SlotManager::advance_state(SlotCtx& ctx, BurstSync::BurstType type) {
    using BT = BurstSync::BurstType;

    ctx.miss_count = 0;

    switch (ctx.state) {
        case SlotState::IDLE:
            if (type == BT::BS_VOICE || type == BT::MS_VOICE) {
                ctx.state = SlotState::VOICE;
            } else if (type == BT::BS_DATA || type == BT::MS_DATA) {
                ctx.state = SlotState::DATA;
            }
            break;

        case SlotState::VOICE:
            // Voice frames continue as VOICE.
            // A DATA sync or extended silence terminates the call.
            if (type == BT::BS_DATA || type == BT::MS_DATA) {
                // Terminator burst also uses a DATA sync in some cases;
                // Phase 4 LC parsing will refine this transition.
                ctx.state = SlotState::IDLE;
            }
            break;

        case SlotState::DATA:
            if (type == BT::BS_VOICE || type == BT::MS_VOICE) {
                ctx.state = SlotState::VOICE;
            }
            break;
    }
}

// ── on_burst() ────────────────────────────────────────────────────────────────

void SlotManager::on_burst(const BurstSync::Burst& burst) {
    // Map timeslot to slot context index
    const int idx = (burst.timeslot == 2) ? 1 : 0;
    SlotCtx&  ctx = slots_[idx];

    ++ctx.count;

    // Advance state machine
    advance_state(ctx, burst.type);

    // Emit Layer 2 event
    SlotEvent ev;
    ev.burst       = burst;
    ev.state       = ctx.state;
    ev.burst_count = ctx.count;

    cb_(ev);
}
