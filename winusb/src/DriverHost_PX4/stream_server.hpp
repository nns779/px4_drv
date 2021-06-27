// stream_server.hpp

#pragma once

#include <cstdint>
#include <memory>
#include <atomic>

#include "server_base.hpp"
#include "pipe_server.hpp"
#include "receiver_base.hpp"
#include "receiver_manager.hpp"

namespace px4 {

class StreamServer final : public px4::ServerBase {
public:
	explicit StreamServer(px4::ReceiverManager &receiver_manager) noexcept;
	~StreamServer() {}

	// cannot copy
	StreamServer(const StreamServer &) = delete;
	StreamServer& operator=(const StreamServer &) = delete;

	// cannot move
	StreamServer(StreamServer &&) = delete;
	StreamServer& operator=(StreamServer &&) = delete;

private:
	class StreamConnection final : public px4::ServerBase::Connection {
	public:
		explicit StreamConnection(ServerBase &parent, std::unique_ptr<px4::PipeServer> &pipe) noexcept;
		~StreamConnection() {}

		// cannot copy
		StreamConnection(const StreamConnection &) = delete;
		StreamConnection& operator=(const StreamConnection &) = delete;

		// cannot move
		StreamConnection(StreamConnection &&) = delete;
		StreamConnection& operator=(StreamConnection &&) = delete;

	private:
		void Worker() noexcept override;
		void StreamWorker(std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf) noexcept;
	};

	px4::ServerBase::Connection* CreateConnection(std::unique_ptr<px4::PipeServer> &pipe) override;
};

} // namespace px4
