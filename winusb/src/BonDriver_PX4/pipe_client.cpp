// pipe_client.cpp

#include "pipe_client.hpp"

#include <system_error>

namespace px4 {

PipeClient::PipeClient() noexcept
	: Pipe()
{

}

PipeClient::~PipeClient()
{

}

bool PipeClient::Connect(const std::wstring &name, const PipeClientConfig &config, HANDLE cancel_event) noexcept
{
	std::wstring path = L"\\\\.\\pipe\\" + name;
	HANDLE pipe_handle;

	while (true) {
		DWORD mode;

		if (!WaitNamedPipeW(path.c_str(), config.timeout)) {
			error_.assign(GetLastError(), std::system_category());
			return false;
		}

		pipe_handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		if (pipe_handle == INVALID_HANDLE_VALUE) {
			DWORD ec = GetLastError();

			if (ec == ERROR_PIPE_BUSY)
				continue;

			error_.assign(ec, std::system_category());
			return false;
		}

		mode = ((config.stream_read) ? PIPE_READMODE_BYTE : PIPE_READMODE_MESSAGE) | PIPE_WAIT;

		if (!SetNamedPipeHandleState(pipe_handle, &mode, nullptr, nullptr)) {
			CloseHandle(pipe_handle);
			error_.assign(GetLastError(), std::system_category());
			return false;
		}

		SetHandle(pipe_handle);

		break;
	}

	return true;
}

} // namespace px4
