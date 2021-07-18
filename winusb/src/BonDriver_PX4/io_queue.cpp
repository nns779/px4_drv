// SPDX-License-Identifier: MIT
/*
 * I/O queue operator (io_queue.cpp)
 *
 * Copyright (c) 2021 nns779
 */

#include "io_queue.hpp"

#include <cstring>
#include <utility>

namespace px4 {

IoQueue::IoQueue(IoOperation io_op, IoProvider &iop, std::size_t buf_size, std::uintptr_t max, std::uintptr_t min, int data_ignore_count)
	: io_op_(io_op),
	iop_(iop),
	buf_size_(buf_size),
	max_(max),
	min_(min),
	total_buf_num_(0),
	current_ofs_(0),
	data_ignore_count_(data_ignore_count),
	data_ignore_remain_(0)
{
	while (min--)
		IncreaseFreeBuffer();
}

IoQueue::~IoQueue()
{
	Stop();
	data_buf_.clear();
	free_buf_.clear();
}

bool IoQueue::Start()
{
	std::lock_guard<std::mutex> lock(mtx_);

	if (th_)
		return true;

	std::unique_lock<std::mutex> free_lock(free_mtx_);
	std::unique_lock<std::mutex> data_lock(data_mtx_);

	for (auto it = free_buf_.begin(); it != free_buf_.end();) {
		if (!*it)
			it = free_buf_.erase(it);
		else
			++it;
	}

	if (current_buf_) {
		free_buf_.push_back(std::move(current_buf_));
		current_ofs_ = 0;
	}

	while (!data_buf_.empty()) {
		auto buf = std::move(data_buf_.front());
		data_buf_.pop_front();

		if (!buf)
			continue;

		free_buf_.push_back(std::move(buf));
	}

	free_lock.unlock();
	free_cond_.notify_all();

	data_lock.unlock();

	th_.reset(new std::thread((io_op_ == IoOperation::READ) ? &px4::IoQueue::ReadWorker : &px4::IoQueue::WriteWorker, this));
	return true;
}

bool IoQueue::Stop()
{
	std::lock_guard<std::mutex> lock(mtx_);

	if (!th_)
		return true;

	if (io_op_ == IoOperation::READ) {
		// stop ReadWorker
		PushBackFreeBuffer(nullptr);
	} else {
		if (current_buf_) {
			// flush
			PushBackDataBuffer(std::move(current_buf_));
			current_ofs_ = 0;
		}

		// stop WriteWorker
		PushBackDataBuffer(nullptr);
	}

	try {
		th_->join();
	} catch (...) {}

	th_.reset();
	return true;
}

std::size_t IoQueue::GetDataBufferCount() const
{
	std::lock_guard<std::mutex> lock(data_mtx_);

	return data_buf_.size();
}

std::size_t IoQueue::GetFreeBufferCount() const
{
	std::lock_guard<std::mutex> lock(free_mtx_);

	return free_buf_.size();
}

bool IoQueue::WaitDataBuffer(std::chrono::milliseconds ms)
{
	std::lock_guard<std::mutex> lock(mtx_);
	std::unique_lock<std::mutex> data_lock(data_mtx_);

	if (data_buf_.size())
		return true;

	if (ms.count()) {
		if (data_cond_.wait_for(data_lock, ms) == std::cv_status::timeout)
			return false;
	} else {
		data_cond_.wait(data_lock);
	}

	return true;
}

void IoQueue::PurgeDataBuffer()
{
	std::lock_guard<std::mutex> lock(mtx_);
	std::unique_lock<std::mutex> free_lock(free_mtx_);
	std::lock_guard<std::mutex> data_lock(data_mtx_);

	if (current_buf_) {
		free_buf_.push_back(std::move(current_buf_));
		current_ofs_ = 0;
	}

	while (!data_buf_.empty()) {
		auto buf = std::move(data_buf_.front());
		data_buf_.pop_front();

		free_buf_.push_back(std::move(buf));
	}

	data_ignore_remain_ = data_ignore_count_;

	free_lock.unlock();
	free_cond_.notify_all();

	return;
}

bool IoQueue::Read(void *buf, std::size_t &size, std::size_t &remain_count, bool blocking)
{
	if (io_op_ != IoOperation::READ)
		return false;

	bool ret = false;
	std::lock_guard<std::mutex> lock(mtx_);
	std::uint8_t *p = static_cast<std::uint8_t*>(buf);
	std::size_t remain = size;

	while (remain) {
		if (!current_buf_) {
			current_buf_ = PopFrontDataBuffer(blocking);
			if (!current_buf_)
				break;
		}

		auto buf = current_buf_.get();
		std::size_t rlen = ((buf->actual_length - current_ofs_) < remain) ? (buf->actual_length - current_ofs_) : remain;

		std::memcpy(p, buf->buf.get() + current_ofs_, rlen);

		current_ofs_ += rlen;
		p += rlen;
		remain -= rlen;

		ret = true;

		if (buf->actual_length == current_ofs_) {
			PushBackFreeBuffer(std::move(current_buf_));
			current_ofs_ = 0;
		}
	}

	size -= remain;
	remain_count = GetDataBufferCount() + ((current_buf_) ? 1 : 0);

	return ret;
}

bool IoQueue::ReadBuffer(void **buf, std::size_t &size, std::size_t &remain_count, bool blocking)
{
	if (io_op_ != IoOperation::READ)
		return false;

	remain_count = 0;

	std::lock_guard<std::mutex> lock(mtx_);

	if (current_buf_ && current_buf_->actual_length == current_ofs_) {
		PushBackFreeBuffer(std::move(current_buf_));
		current_ofs_ = 0;
	}

	if (!current_buf_) {
		current_buf_ = PopFrontDataBuffer(blocking);
		if (!current_buf_)
			return false;
	}

	*buf = current_buf_->buf.get() + current_ofs_;
	size = current_buf_->actual_length - current_ofs_;
	current_ofs_ = current_buf_->actual_length;
	remain_count = GetDataBufferCount();

	return true;
}

bool IoQueue::HaveReadingBuffer()
{
	return !!current_buf_;
}

bool IoQueue::Write(void *buf, std::size_t &size, bool blocking)
{
	// not implemented
	return false;
}

bool IoQueue::IncreaseFreeBuffer()
{
	if (total_buf_num_ >= max_)
		return false;

	auto buf = new IoBuffer;

	buf->buf.reset(new std::uint8_t[buf_size_]);
	buf->actual_length = 0;

	free_buf_.emplace_front(buf);
	total_buf_num_++;

	return true;
}

void IoQueue::DecreaseFreeBuffer()
{
	if (free_buf_.empty() || !free_buf_.front())
		return;

	free_buf_.pop_front();
	total_buf_num_--;

	return;
}

bool IoQueue::PushBackDataBuffer(std::unique_ptr<IoBuffer> &&buf)
{
	std::unique_lock<std::mutex> lock(data_mtx_);

	if (data_ignore_remain_ > 0) {
		data_ignore_remain_--;
		lock.unlock();
		return PushBackFreeBuffer(std::move(buf));
	}

	data_buf_.push_back(std::move(buf));

	lock.unlock();
	data_cond_.notify_all();

	return true;
}

bool IoQueue::PushBackFreeBuffer(std::unique_ptr<IoBuffer> &&buf)
{
	std::unique_lock<std::mutex> lock(free_mtx_);

	free_buf_.push_back(std::move(buf));

	lock.unlock();
	free_cond_.notify_all();

	return true;
}

std::unique_ptr<IoQueue::IoBuffer> IoQueue::PopFrontDataBuffer(bool wait)
{
	std::unique_lock<std::mutex> lock(data_mtx_);

	if (data_buf_.empty()) {
		if (wait) {
			do {
				data_cond_.wait(lock);
			} while (data_buf_.empty());
		} else {
			return nullptr;
		}
	}

	auto buf = std::move(data_buf_.front());
	data_buf_.pop_front();

	if (!buf)
		data_buf_.push_front(nullptr);

	return buf;
}

std::unique_ptr<IoQueue::IoBuffer> IoQueue::PopBackFreeBuffer(bool wait)
{
	std::unique_lock<std::mutex> lock(free_mtx_);

	auto size = free_buf_.size();
	if (!size) {
		if (!IncreaseFreeBuffer()) {
			if (wait) {
				do {
					free_cond_.wait(lock);
				} while (free_buf_.empty());
			} else {
				return nullptr;
			}
		}
	} else if (size > min_) {
		DecreaseFreeBuffer();
	}

	auto buf = std::move(free_buf_.back());
	free_buf_.pop_back();

	if (!buf)
		free_buf_.push_back(nullptr);

	return buf;
}

void IoQueue::ReadWorker()
{
	if (!iop_.Start())
		return;

	while (true) {
		auto buf = PopBackFreeBuffer(true);
		if (!buf) {
			PushBackDataBuffer(nullptr);
			break;
		}

		std::size_t rofs = 0;
		bool quit = false;

		while (rofs < buf_size_) {
			std::size_t rlen = buf_size_ - rofs;

			if (!iop_.Do(buf->buf.get() + rofs, rlen)) {
				quit = true;
				break;
			}

			rofs += rlen;
		}

		buf->actual_length = rofs;
		PushBackDataBuffer(std::move(buf));

		if (quit) {
			PushBackDataBuffer(nullptr);
			break;
		}
	}

	iop_.Stop();
}

void IoQueue::WriteWorker()
{
	if (!iop_.Start())
		return;

	while (true) {
		auto buf = PopFrontDataBuffer(true);
		if (!buf) {
			PushBackFreeBuffer(nullptr);
			break;
		}

		std::size_t len = buf->actual_length, wofs = 0;
		bool quit = false;

		while (wofs < len) {
			std::size_t wlen = len - wofs;

			if (!iop_.Do(buf->buf.get() + wofs, wlen)) {
				quit = true;
				break;
			}

			wofs += wlen;
		}

		PushBackFreeBuffer(std::move(buf));

		if (quit) {
			PushBackFreeBuffer(nullptr);
			break;
		}
	}

	iop_.Stop();
}

} // namespace px4
