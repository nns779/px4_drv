// ctrl_server.hpp

#pragma once

#include <cstdint>
#include <memory>

#include "server_base.hpp"
#include "pipe_server.hpp"
#include "receiver_base.hpp"
#include "receiver_manager.hpp"

namespace px4 {

class CtrlServer final : public px4::ServerBase {
public:
	explicit CtrlServer(px4::ReceiverManager &receiver_manager);
	~CtrlServer() {}

	// cannot copy
	CtrlServer(const CtrlServer &) = delete;
	CtrlServer& operator=(const CtrlServer &) = delete;

	// cannot move
	CtrlServer(CtrlServer &&) = delete;
	CtrlServer& operator=(CtrlServer &&) = delete;

private:
	class CtrlConnection final : public px4::ServerBase::Connection {
	public:
		explicit CtrlConnection(ServerBase &parent, std::unique_ptr<px4::PipeServer> &pipe) noexcept;
		~CtrlConnection() {}

		// cannot copy
		CtrlConnection(const CtrlConnection &) = delete;
		CtrlConnection& operator=(const CtrlConnection &) = delete;

		// cannot move
		CtrlConnection(CtrlConnection &&) = delete;
		CtrlConnection& operator=(CtrlConnection &&) = delete;

	private:
		void Worker() noexcept override;
	};

	px4::ServerBase::Connection* CreateConnection(std::unique_ptr<px4::PipeServer> &pipe) override;
};

} // namespace px4
