// receiver_base.cpp

#include "receiver_base.hpp"

namespace px4 {

ReceiverBase::ReceiverBase()
{
	memset(&params_, 0, sizeof(params_));
	stream_buf_.reset(new StreamBuffer());
}

ReceiverBase::~ReceiverBase()
{

}

bool ReceiverBase::GetParameters(px4::command::ParameterSet &param_set) noexcept
{
	param_set.system = params_.system;
	param_set.freq = params_.freq;

	for (std::uint32_t i = 0; i < param_set.num; i++) {
		switch (param_set.params[i].type) {
		case px4::command::ParameterType::BANDWIDTH:
			param_set.params[i].value = params_.bandwidth;
			break;

		case px4::command::ParameterType::STREAM_ID:
			param_set.params[i].value = params_.stream_id;
			break;

		default:
			// unknown parameter type
			return false;
		}
	}

	return true;
}

bool ReceiverBase::SetParameters(const px4::command::ParameterSet &param_set) noexcept
{
	params_.system = param_set.system;
	params_.freq = param_set.freq;

	for (std::uint32_t i = 0; i < param_set.num; i++) {
		switch (param_set.params[i].type) {
		case px4::command::ParameterType::BANDWIDTH:
			params_.bandwidth = param_set.params[i].value;
			break;

		case px4::command::ParameterType::STREAM_ID:
			params_.stream_id = param_set.params[i].value;
			break;

		default:
			// unknown parameter type
			return false;
		}
	}

	return true;
}

void ReceiverBase::ClearParameters() noexcept
{
	memset(&params_, 0, sizeof(params_));
}

bool ReceiverBase::ReadStats(px4::command::StatSet &stat_set)
{
	for (std::uint32_t i = 0; i < stat_set.num; i++) {
		if (ReadStat(stat_set.data[i].type, stat_set.data[i].value))
			return false;
	}

	return true;
}

ReceiverBase::StreamBuffer::StreamBuffer()
	: write_size_(0),
	threshold_size_(0)
{

}

ReceiverBase::StreamBuffer::~StreamBuffer()
{

}

void ReceiverBase::StreamBuffer::SetThresholdSize(std::size_t size) noexcept
{
	threshold_size_ = size;
}

bool ReceiverBase::StreamBuffer::Alloc(std::size_t size)
{
	return ringbuf_.Alloc(size);
}

void ReceiverBase::StreamBuffer::Start() noexcept
{
	stop_ = false;
	write_size_ = 0;

	ringbuf_.Start();
	return;
}

void ReceiverBase::StreamBuffer::Stop() noexcept
{
	ringbuf_.Stop();

	stop_ = true;
	cond_.notify_all();

	return;
}

void ReceiverBase::StreamBuffer::StopRequest() noexcept
{
	stop_ = true;
	cond_.notify_all();

	return;
}

bool ReceiverBase::StreamBuffer::Write(const void *buf, std::size_t &size) noexcept
{
	bool ret;

	ret = ringbuf_.Write(buf, size);
	write_size_ += size;

	return ret;
}

void ReceiverBase::StreamBuffer::NotifyWrite() noexcept
{
	if (write_size_ >= threshold_size_) {
		cond_.notify_all();
		write_size_ -= threshold_size_;
	}
	return;
}

void ReceiverBase::StreamBuffer::HandleRead(std::size_t buf_size, std::function<bool(const void *buf, std::size_t size)> handler)
{
	stop_ = false;

	std::unique_ptr<std::uint8_t[]> buf(new std::uint8_t[buf_size]);
	std::uint8_t *p = buf.get();
	std::unique_lock<std::mutex> lock(mtx_);

	while (!ringbuf_.IsActive() && !stop_)
		cond_.wait(lock);

	while (true) {
		while (!ringbuf_.GetReadableSize() && !stop_)
			cond_.wait(lock);

		if (stop_)
			break;

		std::size_t size = buf_size;

		if (!ringbuf_.Read(p, size))
			break;

		if (!size)
			continue;

		if (!handler(p, size))
			break;
	}

	return;
}

} // namespace px4
