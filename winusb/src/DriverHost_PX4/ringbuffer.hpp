// ringbuffer.hpp

#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>

namespace px4 {

class RingBuffer final {
public:
	explicit RingBuffer() : RingBuffer(0) {}
	explicit RingBuffer(std::size_t size);
	~RingBuffer();

	bool Alloc(std::size_t size);
	void Reset() noexcept;
	void Start() noexcept;
	void Stop() noexcept;
	bool IsActive() noexcept { return (state_.load()); }
	bool Read(void *buf, std::size_t &size) noexcept;
	bool Write(const void *buf, std::size_t &size) noexcept;
	bool Purge() noexcept;
	std::size_t GetReadableSize() const noexcept { return actual_size_; }
	std::size_t GetWritableSize() const noexcept { return buf_size_ - actual_size_; }

private:
	std::atomic_int state_;
	std::uint8_t *buf_;
	std::size_t buf_size_;
	std::atomic_size_t actual_size_;
	std::atomic_intptr_t head_;	// read
	std::atomic_intptr_t tail_;	// write
	std::atomic_bool wait_;
	std::atomic_intptr_t rw_count_;
	std::mutex wait_lock_;
	std::condition_variable wait_cond_;
};

} // namespace px4
