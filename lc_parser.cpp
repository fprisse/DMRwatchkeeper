#include "lc_parser.h"

// ── Bit extraction helpers ────────────────────────────────────────────────────

uint8_t LcParser::extract_u8(const uint8_t* bits, int offset) {
    uint8_t val = 0;
    for (int i = 0; i < 8; ++i) {
        val = (val << 1) | (bits[offset + i] & 1);
    }
    return val;
}

uint32_t LcParser::extract_u24(const uint8_t* bits, int offset) {
    uint32_t val = 0;
    for (int i = 0; i < 24; ++i) {
        val = (val << 1) | (bits[offset + i] & 1);
    }
    return val;
}

int32_t LcParser::sign_extend(uint32_t val, int nbits) {
    const uint32_t sign_bit = 1u << (nbits - 1);
    if (val & sign_bit) {
        val |= ~((1u << nbits) - 1u);
    }
    return static_cast<int32_t>(val);
}

// ── parse_voice_lc() ─────────────────────────────────────────────────────────

LcResult LcParser::parse_voice_lc(const uint8_t bits[96]) {
    LcResult r;

    // Bit [0]:   PF
    // Bit [1]:   Reserved
    // Bits [2..7]: FLCO (6 bits)
    r.flco = 0;
    for (int i = 0; i < 6; ++i) {
        r.flco = (r.flco << 1) | (bits[2 + i] & 1);
    }

    // Only handle group voice (0x00) and individual voice (0x03)
    if (r.flco != 0x00 && r.flco != 0x03) {
        return r;  // valid = false
    }

    r.group  = (r.flco == 0x00);

    // Bits [8..15]:  FID  (we skip)
    // Bits [16..23]: Service Options (we skip)

    // Bits [24..47]: Destination ID (24 bits)
    r.dst_id = extract_u24(bits, 24);

    // Bits [48..71]: Source ID (24 bits)
    r.src_id = extract_u24(bits, 48);

    // Basic sanity: IDs should be non-zero
    if (r.src_id == 0 && r.dst_id == 0) return r;

    r.valid = true;
    return r;
}

// ── parse_gps() ───────────────────────────────────────────────────────────────

GpsResult LcParser::parse_gps(const uint8_t bits[96]) {
    GpsResult g;

    // Extract 12 bytes from the 96 bits
    uint8_t bytes[12];
    for (int i = 0; i < 12; ++i) {
        bytes[i] = extract_u8(bits, i * 8);
    }

    /*
     * MOTOTRBO short data GPS response.
     *
     * Byte 0: Packet type.
     *   0x00 = immediate position, no reason code
     *   0x20 = immediate position, with reason code
     *   Other values → not a GPS packet, skip.
     *
     * Bytes 1-3: Latitude (24-bit signed, big-endian)
     *   raw value, sign-extended, then: lat = raw * (90.0 / 0x7FFFFF)
     *
     * Bytes 4-6: Longitude (24-bit signed, big-endian)
     *   lon = raw * (180.0 / 0x7FFFFF)
     *
     * Byte 7: Status nibble (upper) + velocity (lower) — not used here
     *
     * Note: Some MOTOTRBO variants encode longitude as 25 bits spanning
     * bytes 4-7.  If coordinates look off by ~0.01°, try the 25-bit parse.
     */

    const uint8_t pkt_type = bytes[0];
    if (pkt_type != 0x00 && pkt_type != 0x20) {
        return g;  // not a position report
    }

    // Latitude: bytes 1-3, 24-bit signed
    const uint32_t raw_lat = ((uint32_t)bytes[1] << 16)
                           | ((uint32_t)bytes[2] <<  8)
                           | (uint32_t)bytes[3];
    const int32_t slat = sign_extend(raw_lat, 24);
    g.lat = slat * (90.0 / 0x7FFFFF);

    // Longitude: bytes 4-6, 24-bit signed
    const uint32_t raw_lon = ((uint32_t)bytes[4] << 16)
                           | ((uint32_t)bytes[5] <<  8)
                           | (uint32_t)bytes[6];
    const int32_t slon = sign_extend(raw_lon, 24);
    g.lon = slon * (180.0 / 0x7FFFFF);

    // Sanity check: coordinates must be within valid range
    if (g.lat < -90.0 || g.lat > 90.0) return g;
    if (g.lon < -180.0 || g.lon > 180.0) return g;

    // Additional sanity: 0,0 is in the ocean off the coast of Africa — likely
    // an uninitialised radio.  Flag as invalid.
    if (g.lat == 0.0 && g.lon == 0.0) return g;

    g.valid = true;
    return g;
}
