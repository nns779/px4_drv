// io_queue.hpp

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <chrono>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <thread>

namespace px4 {

class IoQueue final {
public:
	enum class IoOperation : int {
		READ = 0,
		WRITE
	};

	struct IoBuffer final {
		std::unique_ptr<std::uint8_t[]> buf;
		std::size_t actual_length;
	};

	class IoProvider {
	public:
		IoProvider() noexcept {}
		virtual ~IoProvider() {}

		virtual bool Start() = 0;
		virtual void Stop() = 0;
		virtual bool Do(void *buf, std::size_t &size) = 0;
	};

	IoQueue(IoOperation io_op, IoProvider &iop, std::size_t buf_size, std::uintptr_t max = 32, std::uintptr_t min = 2, int data_ignore_count = 1);
	~IoQueue();

	// cannot copy
	IoQueue(const IoQueue &) = delete;
	IoQueue& operator=(const IoQueue &) = delete;

	bool Start();
	bool Stop();

	std::size_t GetDataBufferCount() const;
	std::size_t GetFreeBufferCount() const;
	bool WaitDataBuffer(std::chrono::milliseconds ms);
	void PurgeDataBuffer();

	bool Read(void *buf, std::size_t &size, std::size_t &remain_count, bool blocking);
	bool ReadBuffer(void **buf, std::size_t &size, std::size_t &remain_count, bool blocking);
	bool HaveReadingBuffer();
	bool Write(void *buf, std::size_t &size, bool blocking);

private:
	bool IncreaseFreeBuffer();
	void DecreaseFreeBuffer();
	bool PushBackDataBuffer(std::unique_ptr<IoBuffer> &&buf);
	bool PushBackFreeBuffer(std::unique_ptr<IoBuffer> &&buf);
	std::unique_ptr<IoBuffer> PopFrontDataBuffer(bool wait);
	std::unique_ptr<IoBuffer> PopBackFreeBuffer(bool wait);

	void ReadWorker();
	void WriteWorker();

	IoOperation io_op_;
	IoProvider &iop_;
	std::size_t buf_size_;
	std::uintptr_t max_;
	std::uintptr_t min_;
	std::mutex mtx_;
	std::size_t total_buf_num_;
	std::deque<std::unique_ptr<IoBuffer>> data_buf_;
	mutable std::mutex data_mtx_;
	std::deque<std::unique_ptr<IoBuffer>> free_buf_;
	mutable std::mutex free_mtx_;
	std::condition_variable data_cond_;
	std::condition_variable free_cond_;
	std::unique_ptr<IoBuffer> current_buf_;
	std::size_t current_ofs_;
	int data_ignore_count_;
	int data_ignore_remain_;
	std::unique_ptr<std::thread> th_;
};

} // namespace px4
