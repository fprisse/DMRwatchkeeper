#pragma once

#include <cstdint>
#include <cstring>

/*
 * BPTC19696
 *
 * Block Product Turbo Code (196,96) decoder.
 * Used for all Full Link Control (LC) payloads in DMR voice bursts.
 *
 * ── What it does ─────────────────────────────────────────────────────────────
 *
 * Takes the 98 payload dibits from a burst (both halves concatenated,
 * sync excluded) and recovers the 96-bit LC message.
 *
 * Pipeline:
 *   98 dibits → 196 bits → deinterleave → 13×15 matrix
 *   → Hamming(15,11) row correction (rows 0–8)
 *   → Hamming(13,9)  column correction (columns 0–14)
 *   → extract bits at rows 0–8, cols 0–10 → 96 output bits
 *
 * ── BPTC(196,96) structure (ETSI TS 102 361-1 §7.2.2) ────────────────────────
 *
 *   Transmitted 196 bits deinterleaved using:
 *     deinterleaved[k] = received[(k × 13) mod 196]   k = 0..195
 *   (inverse of the interleave i(n) = (n × 181) mod 196 applied before tx)
 *
 *   Arranged into a 13×15 matrix (row-major, 195 bits + 1 dummy):
 *
 *          col→  0  1  2  3  4  5  6  7  8  9  10  11 12 13 14
 *    row 0       d  d  d  d  d  d  d  d  d  d   d   p  p  p  p
 *    row 1       d  d  d  d  d  d  d  d  d  d   d   p  p  p  p
 *    ...                                                        
 *    row 8       d  d  d  d  d  d  d  d  d  d   d   p  p  p  p
 *    row 9       c  c  c  c  c  c  c  c  c  c   c   c  c  c  c
 *    row 10      c  c  c  c  c  c  c  c  c  c   c   c  c  c  c
 *    row 11      c  c  c  c  c  c  c  c  c  c   c   c  c  c  c
 *    row 12      c  c  c  c  c  c  c  c  c  c   c   c  c  c  c
 *
 *   'd' = data bits (11 per row × 9 rows = 99, but 3 are dummy → 96 LC bits)
 *   'p' = Hamming(15,11) row parity (4 per row)
 *   'c' = column parity (4 rows × 15 columns = Hamming(13,9) per column)
 *
 *   Data bits in output order: row 0 cols 0–10, row 1 cols 0–10, …,
 *   row 8 cols 0–7 (bits 0–95). Row 8 cols 8–10 are dummy zeros.
 *
 * ── Error correction capability ───────────────────────────────────────────────
 *
 * Row Hamming(15,11): corrects any single-bit error per row.
 * Column Hamming(13,9): corrects any single-bit error per column.
 * Product code: corrects all single-bit errors in the 13×15 matrix.
 *
 * ── Important note on column equations ───────────────────────────────────────
 *
 * The column parity equations are derived from shortened Hamming(15,11)
 * (positions 1,2 removed → Hamming(13,9)).  Verify against ETSI TS 102 361-1
 * Table 9.27 if seeing unexpected decode failures on strong signals.
 */
class BPTC19696 {
public:
    /*
     * decode()
     *
     * Input:  dibits[98]   — 98 payload dibits from BurstSync::Burst::dibits[]
     *                        first 49 = burst first half, next 49 = second half
     * Output: bits_out[96] — recovered 96-bit LC message, MSB first per byte
     *
     * Returns true  if no uncorrectable error was detected.
     * Returns false if a double-bit (or higher) error was detected; output
     *               bits may be partially corrupted — caller should discard.
     */
    bool decode(const uint8_t dibits[98], uint8_t bits_out[96]);

private:
    // ── Internal types ──────────────────────────────────────────────────────
    // All intermediate work in a flat bit array (0 or 1 per element).
    // Using uint8_t avoids masking and is cache-friendly for a 196-element array.

    // Deinterleave: flat 196-bit received array → flat 196-bit matrix order
    static void deinterleave(const uint8_t in[196], uint8_t out[196]);

    // Hamming(15,11): correct one row (15 bits).
    // Returns false if a double-bit error is detected.
    static bool correct_row(uint8_t row[15]);

    // Hamming(13,9): correct one column (13 bits).
    // Returns false if a double-bit error is detected.
    static bool correct_col(uint8_t col[13]);

    // Error position lookup for Hamming(15,11): syndrome (1..15) → array index
    // Syndrome 0 = no error. syndrome>15 should not occur.
    static constexpr int8_t kRow15ErrPos[16] = {
        -1,   // 0: no error
        11,   // 1: p0  (position 1)
        12,   // 2: p1  (position 2)
         0,   // 3: d0  (position 3)
        13,   // 4: p2  (position 4)
         1,   // 5: d1  (position 5)
         2,   // 6: d2  (position 6)
         3,   // 7: d3  (position 7)
        14,   // 8: p3  (position 8)
         4,   // 9: d4  (position 9)
         5,   // 10: d5 (position 10)
         6,   // 11: d6 (position 11)
         7,   // 12: d7 (position 12)
         8,   // 13: d8 (position 13)
         9,   // 14: d9 (position 14)
        10,   // 15: d10(position 15)
    };

    // Error position lookup for Hamming(13,9): syndrome → array index
    // Same structure, shortened code has no d9/d10 → positions 14,15 map to -1
    static constexpr int8_t kCol13ErrPos[16] = {
        -1,   // 0: no error
         9,   // 1: p0  (col row 9)
        10,   // 2: p1  (col row 10)
         0,   // 3: d0
        11,   // 4: p2  (col row 11)
         1,   // 5: d1
         2,   // 6: d2
         3,   // 7: d3
        12,   // 8: p3  (col row 12)
         4,   // 9: d4
         5,   // 10: d5
         6,   // 11: d6
         7,   // 12: d7
         8,   // 13: d8
        -1,   // 14: no corresponding bit (shortened)
        -1,   // 15: no corresponding bit (shortened)
    };
};
