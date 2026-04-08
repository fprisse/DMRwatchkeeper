#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

/*
 * RingBuffer
 *
 * Single-Producer Single-Consumer lock-free circular buffer.
 * The RTL-SDR callback thread writes; the demodulator thread reads.
 *
 * Stores raw uint8_t IQ bytes exactly as librtlsdr delivers them:
 *   [ I0, Q0, I1, Q1, ... ]
 * Each sample pair is 2 bytes, values in [0,255], DC-offset at 127.
 *
 * Size must be a power of two to allow cheap modulo via bitmask.
 */

class RingBuffer {
public:
    /*
     * capacity_bytes  — must be a power of two.
     *                   Recommended: 4 * 1024 * 1024 (4 MB) which holds
     *                   ~2 seconds of IQ at 250 kSPS with margin.
     */
    explicit RingBuffer(size_t capacity_bytes);
    ~RingBuffer() = default;

    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // ── Producer API (RTL-SDR callback thread) ─────────────────────────────

    /*
     * write()
     * Copy 'len' bytes from 'data' into the buffer.
     * Returns the number of bytes actually written (may be less than len
     * if the buffer is nearly full — caller should monitor for overruns).
     */
    size_t write(const uint8_t* data, size_t len);

    // ── Consumer API (demodulator thread) ──────────────────────────────────

    /*
     * read()
     * Copy up to 'len' bytes into 'dest'.
     * Returns the number of bytes actually read.
     */
    size_t read(uint8_t* dest, size_t len);

    /*
     * available()
     * Number of bytes ready to be read.
     */
    size_t available() const;

    /*
     * capacity()
     * Total buffer size in bytes.
     */
    size_t capacity() const { return capacity_; }

    /*
     * overrun_count()
     * Number of write() calls that were forced to drop bytes due to a full buffer.
     * Useful for monitoring pipeline health.
     */
    uint64_t overrun_count() const { return overruns_.load(std::memory_order_relaxed); }

    /*
     * reset()
     * Flush all data. NOT thread-safe — call only when both threads are paused.
     */
    void reset();

private:
    const size_t          capacity_;
    const size_t          mask_;      // capacity_ - 1, for fast modulo
    std::unique_ptr<uint8_t[]> buf_;

    // head_ = next write position (producer owns)
    // tail_ = next read  position (consumer owns)
    // Both are monotonically increasing; wrap via mask_.
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

    std::atomic<uint64_t> overruns_{0};
};
