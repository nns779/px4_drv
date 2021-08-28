// pipe_server.cpp

#include "pipe_server.hpp"

#include <system_error>

#include "security_attributes.hpp"

namespace px4 {

PipeServer::PipeServer() noexcept
	: Pipe()
{

}

PipeServer::~PipeServer()
{
	if (!IsConnected())
		return;

	FlushFileBuffers(handle_);
	DisconnectNamedPipe(handle_);
}

bool PipeServer::Accept(const std::wstring &name, const PipeServerConfig &config, HANDLE ready_event, HANDLE cancel_event, DWORD timeout) noexcept
{
	if (IsConnected())
		return false;

	bool ret = false;
	std::wstring path = L"\\\\.\\pipe\\" + name;
	OVERLAPPED ol = { 0 };
	DWORD mode = 0;
	HANDLE pipe_handle = nullptr;

	ol.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!ol.hEvent) {
		error_.assign(GetLastError(), std::system_category());
		goto exit;
	}

	mode |= (config.stream_pipe) ? PIPE_TYPE_BYTE : PIPE_TYPE_MESSAGE;
	mode |= (config.stream_read) ? PIPE_READMODE_BYTE : PIPE_READMODE_MESSAGE;
	mode |= PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;

	try {
		SecurityAttributes sa(GENERIC_READ | GENERIC_WRITE);

		pipe_handle = CreateNamedPipeW(
			path.c_str(),
			PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
			mode,
			PIPE_UNLIMITED_INSTANCES,
			static_cast<DWORD>(config.out_buffer_size),
			static_cast<DWORD>(config.in_buffer_size),
			config.default_timeout,
			sa.Get());
		if (pipe_handle == INVALID_HANDLE_VALUE) {
			error_.assign(GetLastError(), std::system_category());
			goto exit;
		}
	} catch (SecurityAttributesError &e) {
		error_.assign(GetLastError(), std::system_category());
		goto exit;
	}

	if (ready_event)
		SetEvent(ready_event);

	if (ConnectNamedPipe(pipe_handle, &ol)) {
		ret = true;
		goto exit;
	} else {
		DWORD err = GetLastError();

		if (err == ERROR_IO_PENDING || err == ERROR_PIPE_LISTENING) {
			HANDLE handles[2] = { ol.hEvent, cancel_event };
			DWORD res, t;

			res = WaitForMultipleObjects((cancel_event) ? 2 : 1, handles, FALSE, INFINITE);
			if (res == WAIT_OBJECT_0 && GetOverlappedResult(pipe_handle, &ol, &t, TRUE))
				ret = true;
			else
				error_.assign(err, std::system_category());
		} else if (err == ERROR_PIPE_CONNECTED) {
			ret = true;
		} else {
			error_.assign(err, std::system_category());
		}
	}

exit:
	if (ret) {
		SetHandle(pipe_handle);
	} else {
		if (pipe_handle && pipe_handle != INVALID_HANDLE_VALUE)
			CloseHandle(pipe_handle);
	}

	if (ol.hEvent)
		CloseHandle(ol.hEvent);

	return ret;
}

} // namespace px4
