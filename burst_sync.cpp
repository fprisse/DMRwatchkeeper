#include "burst_sync.h"

#include <cstring>
#include <stdexcept>

// ── Constructor ──────────────────────────────────────────────────────────────

BurstSync::BurstSync(BurstCallback cb, int channel_idx)
    : cb_(std::move(cb))
    , channel_idx_(channel_idx)
{
    if (!cb_) {
        throw std::invalid_argument("BurstSync: callback must not be null");
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

int BurstSync::hamming48(uint64_t a, uint64_t b) {
    // Count differing bits in the lower 48 bits
    return __builtin_popcountll((a ^ b) & kMask48);
}

BurstSync::BurstType BurstSync::match_sync(uint64_t word) const {
    const uint64_t w = word & kMask48;

    if (hamming48(w, kSyncBsVoice) <= kMaxErrors) return BurstType::BS_VOICE;
    if (hamming48(w, kSyncMsVoice) <= kMaxErrors) return BurstType::MS_VOICE;
    if (hamming48(w, kSyncBsData)  <= kMaxErrors) return BurstType::BS_DATA;
    if (hamming48(w, kSyncMsData)  <= kMaxErrors) return BurstType::MS_DATA;

    return BurstType::UNKNOWN;
}

/*
 * extract_first_half()
 *
 * At the moment sync is detected, hist_head_ has already been incremented
 * past the last sync dibit.  The 49 first-half dibits sit immediately before
 * the 24 sync dibits in the history buffer.
 *
 * Memory layout (reading backwards from hist_head_):
 *   [hist_head_ - 1]      = last dibit of sync word    (position 0 from end)
 *   [hist_head_ - 24]     = first dibit of sync word
 *   [hist_head_ - 25]     = last dibit of first half
 *   [hist_head_ - 73]     = first dibit of first half  ← start here
 *
 * Read 49 consecutive entries forward from that position.
 */
void BurstSync::extract_first_half(uint8_t* out) const {
    // Start offset from head (how far back to go for the first first-half dibit)
    // = 24 (sync) + 49 (first half) = 73 positions back from hist_head_
    const int start = (hist_head_ - 73 + kHistLen * 2) % kHistLen;

    for (int i = 0; i < 49; ++i) {
        out[i] = hist_[(start + i) % kHistLen];
    }
}

const char* BurstSync::burst_type_str(BurstType t) {
    switch (t) {
        case BurstType::BS_VOICE: return "BS_VOICE";
        case BurstType::MS_VOICE: return "MS_VOICE";
        case BurstType::BS_DATA:  return "BS_DATA";
        case BurstType::MS_DATA:  return "MS_DATA";
        default:                  return "UNKNOWN";
    }
}

// ── push_dibit() ─────────────────────────────────────────────────────────────

void BurstSync::push_dibit(uint8_t dibit) {
    const uint8_t d = dibit & 0x03;

    // Always update the history buffer
    hist_[hist_head_] = d;
    hist_head_ = (hist_head_ + 1) % kHistLen;

    // Always feed the shift register (needed for sync detection even when collecting)
    sync_reg_ = ((sync_reg_ << 2) | d) & kMask48;

    // ── HUNTING state ──────────────────────────────────────────────────────
    if (state_ == State::HUNTING) {
        const BurstType bt = match_sync(sync_reg_);

        if (bt != BurstType::UNKNOWN) {
            // Sync matched — begin assembling a burst
            ++sync_matches_;

            current_           = Burst{};
            current_.type      = bt;
            current_.channel   = channel_idx_;
            current_.sync_bits = sync_reg_ & kMask48;
            current_.timeslot  = (ts_toggle_++ & 1) ? 2 : 1;

            // Recover the 49 first-half dibits from history
            extract_first_half(current_.dibits);  // fills [0..48]

            collect_idx_ = 0;
            state_       = State::COLLECTING;
        }
        return;
    }

    // ── COLLECTING state ───────────────────────────────────────────────────
    // Accumulate second-half dibits into dibits[49..97]
    if (collect_idx_ < 49) {
        current_.dibits[49 + collect_idx_] = d;
    }
    ++collect_idx_;

    // After 49 payload dibits + 10 guard dibits = 59 total, burst is complete
    if (collect_idx_ >= 59) {
        // Emit the completed burst
        ++burst_count_;
        cb_(current_);

        // Return to hunting — do NOT reset sync_reg_ so we catch
        // an immediately following sync word without a gap
        state_ = State::HUNTING;
    }
}
