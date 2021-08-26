// pipe_client.hpp

#pragma once

#include <cstdint>
#include <string>

#include <windows.h>

#include "pipe.hpp"

namespace px4 {

class PipeClient final : public px4::Pipe {
public:
	struct PipeClientConfig {
		bool stream_read;
		std::uint32_t timeout;
	};

	PipeClient() noexcept;
	~PipeClient();

	bool Connect(const std::wstring &name, const PipeClientConfig &config, HANDLE cancel_event) noexcept;
};

} // namespace px4
