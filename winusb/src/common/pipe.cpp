// pipe.cpp

#include "pipe.hpp"

namespace px4 {

Pipe::Pipe(HANDLE handle) noexcept
	: handle_(handle),
	ol_event_()
{

}

Pipe::~Pipe()
{
	if (!IsConnected())
		return;

	CloseHandle(handle_);
	handle_ = INVALID_HANDLE_VALUE;

	for (int i = 0; i < 2; i++) {
		if (ol_event_[i]) {
			CloseHandle(ol_event_[i]);
			ol_event_[i] = nullptr;
		}
	}
}

bool Pipe::Read(void *buf, std::size_t size, std::size_t &return_size) noexcept
{
	return Read(buf, size, return_size, nullptr);
}

bool Pipe::Read(void *buf, std::size_t size, std::size_t &return_size, HANDLE cancel_event) noexcept
{
	if (!ol_event_[0]) {
		ol_event_[0] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!ol_event_[0]) {
			error_.assign(GetLastError(), std::system_category());
			return false;
		}
	}

	DWORD read = 0, err;
	OVERLAPPED ol = { 0 };

	ResetEvent(ol_event_[0]);
	ol.hEvent = ol_event_[0];

	if (ReadFile(handle_, buf, static_cast<DWORD>(size), &read, &ol)) {
		return_size = read;
		return true;
	}

	err = GetLastError();
	if (err != ERROR_IO_PENDING) {
		error_.assign(err, std::system_category());
		return false;
	}

	HANDLE events[2] = { ol_event_[0], cancel_event };
	DWORD res;

	res = WaitForMultipleObjects((cancel_event) ? 2 : 1, events, FALSE, INFINITE);
	if (res == WAIT_FAILED) {
		error_.assign(GetLastError(), std::system_category());
		return false;
	}

	if (res != WAIT_OBJECT_0) {
		error_.assign(ECANCELED, std::generic_category());
		return false;
	}

	if (!GetOverlappedResult(handle_, &ol, &read, TRUE)) {
		error_.assign(GetLastError(), std::system_category());
		return false;
	}

	return_size = read;

	return true;
}

bool Pipe::Write(const void *buf, std::size_t size, std::size_t &return_size) noexcept
{
	return Write(buf, size, return_size, nullptr);
}

bool Pipe::Write(const void *buf, std::size_t size, std::size_t &return_size, HANDLE cancel_event) noexcept
{
	if (!ol_event_[1]) {
		ol_event_[1] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!ol_event_[1]) {
			error_.assign(GetLastError(), std::system_category());
			return false;
		}
	}

	DWORD written = 0, err;
	OVERLAPPED ol = { 0 };

	ResetEvent(ol_event_[1]);
	ol.hEvent = ol_event_[1];

	if (WriteFile(handle_, buf, static_cast<DWORD>(size), &written, &ol)) {
		return_size = written;
		return true;
	}

	err = GetLastError();
	if (err != ERROR_IO_PENDING) {
		error_.assign(err, std::system_category());
		return false;
	}

	HANDLE events[2] = { ol_event_[1], cancel_event };
	DWORD res;

	res = WaitForMultipleObjects((cancel_event) ? 2 : 1, events, FALSE, INFINITE);
	if (res == WAIT_FAILED) {
		error_.assign(GetLastError(), std::system_category());
		return false;
	}

	if (res != WAIT_OBJECT_0) {
		error_.assign(ECANCELED, std::generic_category());
		return false;
	}

	if (!GetOverlappedResult(handle_, &ol, &written, TRUE)) {
		error_.assign(GetLastError(), std::system_category());
		return false;
	}

	return_size = written;

	return true;
}

bool Pipe::Call(void *buf, std::size_t size) noexcept
{
	return Call(buf, size, buf, size);
}

bool Pipe::Call(const void *buf_in, std::size_t size_in, void *buf_out, std::size_t size_out) noexcept
{
	std::size_t ret_size;

	if (!Write(buf_in, size_in, ret_size) || ret_size != size_in)
		return false;

	if (!Read(buf_out, size_out, ret_size) || ret_size != size_out)
		return false;

	return true;
}

} // namespace px4
