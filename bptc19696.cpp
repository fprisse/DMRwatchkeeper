#include "bptc19696.h"

// ── constexpr table definitions (required in C++17 for ODR) ──────────────────
constexpr int8_t BPTC19696::kRow15ErrPos[16];
constexpr int8_t BPTC19696::kCol13ErrPos[16];

// ── deinterleave ──────────────────────────────────────────────────────────────

void BPTC19696::deinterleave(const uint8_t in[196], uint8_t out[196]) {
    // Inverse of transmit interleaving i(n) = (n × 181) mod 196
    // Recovery: out[k] = in[(k × 13) mod 196]
    for (int k = 0; k < 196; ++k) {
        out[k] = in[(k * 13) % 196];
    }
}

// ── Hamming(15,11) row correction ─────────────────────────────────────────────

bool BPTC19696::correct_row(uint8_t r[15]) {
    // Data bits:   r[0]..r[10]  (d0..d10)
    // Parity bits: r[11]..r[14] (p0..p3)
    //
    // Parity equations (from ETSI TS 102 361-1, Hamming(15,11)):
    //   p0 covers: d0,d1,d3,d4,d6,d8,d10
    //   p1 covers: d0,d2,d3,d5,d6,d9,d10
    //   p2 covers: d1,d2,d3,d7,d8,d9,d10
    //   p3 covers: d4,d5,d6,d7,d8,d9,d10

    const uint8_t c0 = r[0]^r[1]^r[3]^r[4]^r[6]^r[8]^r[10];
    const uint8_t c1 = r[0]^r[2]^r[3]^r[5]^r[6]^r[9]^r[10];
    const uint8_t c2 = r[1]^r[2]^r[3]^r[7]^r[8]^r[9]^r[10];
    const uint8_t c3 = r[4]^r[5]^r[6]^r[7]^r[8]^r[9]^r[10];

    // Syndrome: XOR computed parity with received parity
    const int s = (c0 ^ r[11])
                | ((c1 ^ r[12]) << 1)
                | ((c2 ^ r[13]) << 2)
                | ((c3 ^ r[14]) << 3);

    if (s == 0) return true;  // no error

    const int8_t pos = kRow15ErrPos[s & 0xF];
    if (pos < 0) return false;  // double-bit or higher — uncorrectable

    r[pos] ^= 1;  // flip the erroneous bit
    return true;
}

// ── Hamming(13,9) column correction ──────────────────────────────────────────

bool BPTC19696::correct_col(uint8_t c[13]) {
    // Data bits:   c[0]..c[8]   (rows 0-8)
    // Parity bits: c[9]..c[12]  (rows 9-12)
    //
    // Shortened Hamming(15,11) → Hamming(13,9):
    // Same parity equations but data only extends to d8 (d9,d10 absent)

    const uint8_t s0 = c[0]^c[1]^c[3]^c[4]^c[6]^c[8];
    const uint8_t s1 = c[0]^c[2]^c[3]^c[5]^c[6];
    const uint8_t s2 = c[1]^c[2]^c[3]^c[7]^c[8];
    const uint8_t s3 = c[4]^c[5]^c[6]^c[7]^c[8];

    const int syn = (s0 ^ c[9])
                  | ((s1 ^ c[10]) << 1)
                  | ((s2 ^ c[11]) << 2)
                  | ((s3 ^ c[12]) << 3);

    if (syn == 0) return true;

    const int8_t pos = kCol13ErrPos[syn & 0xF];
    if (pos < 0) return false;  // uncorrectable

    c[pos] ^= 1;
    return true;
}

// ── decode() ──────────────────────────────────────────────────────────────────

bool BPTC19696::decode(const uint8_t dibits[98], uint8_t bits_out[96]) {

    // ── Step 1: convert 98 dibits → 196 bits ─────────────────────────────────
    // Each dibit d encodes two bits: MSB = (d>>1)&1, LSB = d&1
    uint8_t received[196];
    for (int k = 0; k < 98; ++k) {
        received[2 * k]     = (dibits[k] >> 1) & 1;
        received[2 * k + 1] = (dibits[k]     ) & 1;
    }

    // ── Step 2: deinterleave → 196-bit matrix order ───────────────────────────
    uint8_t matrix_flat[196];
    deinterleave(received, matrix_flat);
    // matrix_flat[r*15 + c] = bit at row r, column c
    // Last entry (index 195) is the dummy padding bit → ignore

    // Helper lambdas for row/column access
    auto mat = [&](int r, int c) -> uint8_t& {
        return matrix_flat[r * 15 + c];
    };

    // ── Step 3: Hamming(15,11) row correction on rows 0–8 ────────────────────
    bool ok = true;
    for (int r = 0; r < 9; ++r) {
        uint8_t row[15];
        for (int c = 0; c < 15; ++c) row[c] = mat(r, c);

        if (!correct_row(row)) {
            ok = false;  // detected but uncorrectable in this row
        }

        for (int c = 0; c < 15; ++c) mat(r, c) = row[c];
    }

    // ── Step 4: Hamming(13,9) column correction on columns 0–14 ─────────────
    for (int c = 0; c < 15; ++c) {
        uint8_t col[13];
        for (int r = 0; r < 13; ++r) col[r] = mat(r, c);

        if (!correct_col(col)) {
            ok = false;
        }

        for (int r = 0; r < 13; ++r) mat(r, c) = col[r];
    }

    // ── Step 5: Optional second-pass row correction ───────────────────────────
    // Column correction may have fixed an error that causes a row syndrome.
    // A second row pass cleans up residual inconsistencies.
    for (int r = 0; r < 9; ++r) {
        uint8_t row[15];
        for (int c = 0; c < 15; ++c) row[c] = mat(r, c);
        correct_row(row);  // result of second pass ignored for ok flag
        for (int c = 0; c < 15; ++c) mat(r, c) = row[c];
    }

    // ── Step 6: extract 96 data bits ─────────────────────────────────────────
    // Data sits at rows 0–8, cols 0–10 (99 positions).
    // Positions 96, 97, 98 (row 8, cols 8–10) are dummy zeros.
    int bit_idx = 0;
    for (int r = 0; r < 9 && bit_idx < 96; ++r) {
        for (int c = 0; c < 11 && bit_idx < 96; ++c) {
            bits_out[bit_idx++] = mat(r, c);
        }
    }

    return ok;
}
