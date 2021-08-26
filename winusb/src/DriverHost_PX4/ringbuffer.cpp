// ringbuffer.cpp

#include "ringbuffer.hpp"

#include <cstring>

#include <windows.h>

namespace px4 {

RingBuffer::RingBuffer(std::size_t size)
	: state_(0),
	buf_(nullptr),
	actual_size_(0),
	head_(0),
	tail_(0),
	wait_(false),
	rw_count_(0),
	wait_lock_(),
	wait_cond_()
{
	Alloc(size);
}

RingBuffer::~RingBuffer()
{
	Stop();

	if (buf_)
		VirtualFree(buf_, 0, MEM_RELEASE);
}

bool RingBuffer::Alloc(std::size_t size)
{
	if (state_)
		return false;

	if (buf_ && buf_size_ != size) {
		VirtualFree(buf_, 0, MEM_RELEASE);
		buf_ = nullptr;
		buf_size_ = 0;
	}
	
	if (!buf_ && size) {
		buf_ = reinterpret_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		if (!buf_)
			return false;

		buf_size_ = size;
	}

	Reset();

	return true;
}

void RingBuffer::Reset() noexcept
{
	actual_size_ = 0;
	head_ = 0;
	tail_ = 0;
}

void RingBuffer::Start() noexcept
{
	int expected = 0;

	state_.compare_exchange_strong(expected, 1);
}

void RingBuffer::Stop() noexcept
{
	state_ = 0;
}

bool RingBuffer::Read(void *buf, std::size_t &size) noexcept
{
#if 0
	int expected_state = 1;

	state_.compare_exchange_strong(expected_state, 2);
#endif

	if (wait_) {
		size = 0;
		return true;
	}

	++rw_count_;

	std::size_t actual_size = actual_size_;
	std::intptr_t head = head_;
	std::size_t buf_size = buf_size_;
	std::size_t read_size = (size <= actual_size) ? size : actual_size;

	if (read_size) {
		std::size_t tmp = (head + read_size <= buf_size) ? read_size : (buf_size - head);

		memcpy(buf, buf_ + head, tmp);

		if (tmp < read_size) {
			memcpy(reinterpret_cast<std::uint8_t *>(buf) + tmp, buf_, read_size - tmp);
			head = read_size - tmp;
		} else {
			head = (head + read_size == buf_size) ? 0 : (head + read_size);
		}

		head_ = head;
		actual_size_ -= read_size;
	}

	if (!--rw_count_ && wait_)
		wait_cond_.notify_all();

	size = read_size;

	return true;
}

bool RingBuffer::Write(const void *buf, std::size_t &size) noexcept
{
#if 0
	if (state_ != 2)
		return false;
#else
	if (!state_) {
		size = 0;
		return false;
	}
#endif

	if (wait_) {
		size = 0;
		return true;
	}

	++rw_count_;

	std::size_t actual_size = actual_size_;
	std::intptr_t tail = tail_;
	std::size_t buf_size = buf_size_;
	std::size_t write_size = (actual_size + size <= buf_size) ? size : (buf_size - actual_size);

	if (write_size) {
		std::size_t tmp = (tail + write_size <= buf_size) ? write_size : (buf_size - tail);

		std::memcpy(buf_ + tail, buf, tmp);

		if (tmp < write_size) {
			std::memcpy(buf_, reinterpret_cast<const std::uint8_t *>(buf) + tmp, write_size - tmp);
			tail = write_size - tmp;
		} else {
			tail = (tail + write_size == buf_size) ? 0 : (tail + write_size);
		}

		tail_ = tail;
		actual_size_ += write_size;
	}

	if (!--rw_count_ && wait_)
		wait_cond_.notify_all();

	bool ret = (size == write_size);

	size = write_size;
	return ret;
}

bool RingBuffer::Purge() noexcept
{
	try {
		bool expected = false;

		if (!wait_.compare_exchange_strong(expected, true))
			return false;

		{
			std::unique_lock<std::mutex> lock(wait_lock_);
			wait_cond_.wait(lock, [this] {
				return (rw_count_ == 0);
			});
		}

		Reset();
		wait_ = false;
	} catch (...) {
		return false;
	}

	return true;
}

} // namespace px4
