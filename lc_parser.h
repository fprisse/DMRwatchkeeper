#pragma once

#include <cstdint>
#include <cmath>

/*
 * LcParser
 *
 * Parses the 96-bit LC (Link Control) payload produced by BPTC19696.
 *
 * ── Full LC structure (ETSI TS 102 361-1 Table 9.30) ─────────────────────────
 *
 *   Bits [0]      PF       Protected Flag (1=RS protected LC)
 *   Bits [1]      Reserved
 *   Bits [2..7]   FLCO     Full Link Control Opcode
 *   Bits [8..15]  FID      Feature set ID
 *   Bits [16..23] SvcOpts  Service Options
 *   Bits [24..47] DstID    Destination ID (24 bits) — talkgroup or radio
 *   Bits [48..71] SrcID    Source ID (24 bits)      — transmitting radio
 *   Bits [72..95] CRC      RS(12,9) parity (not decoded here)
 *
 * FLCO values relevant to us:
 *   0x00  Group Voice Call Channel User
 *   0x03  Unit-to-Unit Voice Call Channel User (individual call)
 *
 * ── GPS / Location (ETSI TS 102 361-4 + Motorola MOTOTRBO extension) ────────
 *
 * GPS data arrives in DATA bursts, not voice bursts.
 * MOTOTRBO uses a proprietary compressed format in short data blocks.
 *
 * The compressed position report (most common MOTOTRBO format):
 *   Byte 0:     Packet type (0x00 = position report)
 *   Bytes 1–3:  Latitude  (24-bit signed integer)
 *   Bytes 4–6:  Longitude (24-bit signed integer, see note)
 *   Byte 7:     Status/velocity
 *
 * Coordinate formulas (from ETSI TS 102 361-4 §7.2.1 and dmr_utils):
 *   lat = raw_lat * (90.0  / 0x7FFFFF)  where raw_lat is sign-extended 24-bit
 *   lon = raw_lon * (180.0 / 0x7FFFFF)  where raw_lon is sign-extended 24-bit
 *
 * NOTE: The exact byte layout varies between MOTOTRBO firmware versions and
 * between LRRP and Short Data GPS formats.  The decoder here targets the most
 * common MOTOTRBO "immediate position" response format.  Verify against field
 * captures if coordinate values look wrong.
 */

struct LcResult {
    bool    valid    = false;
    uint8_t flco     = 0xFF;    // Full LC Opcode
    bool    group    = false;   // true = talkgroup call, false = individual
    uint32_t src_id  = 0;       // Source (transmitting) radio ID
    uint32_t dst_id  = 0;       // Destination talkgroup or radio ID
};

struct GpsResult {
    bool   valid = false;
    double lat   = 0.0;
    double lon   = 0.0;
};

class LcParser {
public:
    /*
     * parse_voice_lc()
     *
     * Parse a 96-bit LC payload (from BPTC19696) from a voice burst.
     * Extracts Source ID, Destination ID, and group flag.
     *
     * bits[96]: array of individual bits (0 or 1), MSB first.
     */
    static LcResult parse_voice_lc(const uint8_t bits[96]);

    /*
     * parse_gps()
     *
     * Attempt to extract GPS coordinates from the 96-bit payload
     * of a DATA burst.
     *
     * Returns a valid GpsResult only if the payload matches the expected
     * MOTOTRBO compressed position format (packet type byte = 0x00 or 0x20).
     *
     * bits[96]: individual bits from BPTC19696 output.
     */
    static GpsResult parse_gps(const uint8_t bits[96]);

private:
    // Extract a 24-bit value from a bit array at a given bit offset, big-endian
    static uint32_t extract_u24(const uint8_t* bits, int offset);

    // Extract a byte from a bit array at a given bit offset
    static uint8_t extract_u8(const uint8_t* bits, int offset);

    // Sign-extend a value from nbits to int32_t
    static int32_t sign_extend(uint32_t val, int nbits);
};
