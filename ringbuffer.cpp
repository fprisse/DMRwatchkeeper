#include "ringbuffer.h"

#include <cassert>
#include <stdexcept>

// ── Helpers ──────────────────────────────────────────────────────────────────

static bool is_power_of_two(size_t v) {
    return v && !(v & (v - 1));
}

// ── Constructor ──────────────────────────────────────────────────────────────

RingBuffer::RingBuffer(size_t capacity_bytes)
    : capacity_(capacity_bytes)
    , mask_(capacity_bytes - 1)
    , buf_(std::make_unique<uint8_t[]>(capacity_bytes))
{
    if (!is_power_of_two(capacity_bytes)) {
        throw std::invalid_argument("RingBuffer: capacity must be a power of two");
    }
}

// ── Producer ─────────────────────────────────────────────────────────────────

size_t RingBuffer::write(const uint8_t* data, size_t len) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);

    const size_t free_space = capacity_ - (head - tail);

    if (len > free_space) {
        // Drop the excess; count the overrun
        overruns_.fetch_add(1, std::memory_order_relaxed);
        len = free_space;
    }

    if (len == 0) return 0;

    // The data may wrap around the end of the physical buffer — handle in
    // two chunks.
    const size_t write_pos  = head & mask_;
    const size_t first_chunk = std::min(len, capacity_ - write_pos);

    std::memcpy(buf_.get() + write_pos, data, first_chunk);

    if (first_chunk < len) {
        // Wrap-around: copy remainder from the start of the buffer
        std::memcpy(buf_.get(), data + first_chunk, len - first_chunk);
    }

    // Publish the write — release so the consumer sees the bytes
    head_.store(head + len, std::memory_order_release);

    return len;
}

// ── Consumer ─────────────────────────────────────────────────────────────────

size_t RingBuffer::read(uint8_t* dest, size_t len) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);

    const size_t avail = head - tail;
    if (len > avail) len = avail;
    if (len == 0) return 0;

    const size_t read_pos   = tail & mask_;
    const size_t first_chunk = std::min(len, capacity_ - read_pos);

    std::memcpy(dest, buf_.get() + read_pos, first_chunk);

    if (first_chunk < len) {
        std::memcpy(dest + first_chunk, buf_.get(), len - first_chunk);
    }

    tail_.store(tail + len, std::memory_order_release);

    return len;
}

size_t RingBuffer::available() const {
    const size_t head = head_.load(std::memory_order_acquire);
    const size_t tail = tail_.load(std::memory_order_acquire);
    return head - tail;
}

void RingBuffer::reset() {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    overruns_.store(0, std::memory_order_relaxed);
}
