// server_base.hpp

#pragma once

#include <cstdint>
#include <memory>
#include <deque>
#include <mutex>
#include <thread>
#include <system_error>

#include <windows.h>

#include "pipe_server.hpp"
#include "receiver_manager.hpp"

namespace px4 {

class ServerBase {
public:
	explicit ServerBase(const std::wstring &pipe_name, px4::ReceiverManager &receiver_manager) noexcept;
	virtual ~ServerBase();

	// cannot copy
	ServerBase(const ServerBase &) = delete;
	ServerBase& operator=(const ServerBase &) = delete;

	// cannot move
	ServerBase(ServerBase &&) = delete;
	ServerBase& operator=(ServerBase &&) = delete;

	bool Start() noexcept;
	bool Stop() noexcept;

	std::size_t GetActiveConnectionCount() const noexcept;

	const std::error_condition& GetError() const noexcept { return error_; }

protected:
	class Connection {
	public:
		explicit Connection(ServerBase &parent, std::unique_ptr<px4::PipeServer> &pipe) noexcept;
		virtual ~Connection();

		// cannot copy
		Connection(const Connection &) = delete;
		Connection& operator=(const Connection &) = delete;

		// cannot move
		Connection(Connection &&) = delete;
		Connection& operator=(Connection &&) = delete;

		bool Start() noexcept;
		bool Stop() noexcept;

		const std::error_condition& GetError() const noexcept { return error_; }

	protected:
		virtual void Worker() noexcept {};

		ServerBase &parent_;
		std::unique_ptr<PipeServer> conn_;
		px4::PipeServer::PipeServerConfig config_;
		px4::ReceiverManager &receiver_manager_;
		HANDLE quit_event_;

		std::error_condition error_;
		std::unique_ptr<std::thread> th_;
	};

	virtual Connection* CreateConnection(std::unique_ptr<px4::PipeServer> &pipe) = 0;
	void RemoveConnection(Connection *conn, bool destruct) noexcept;
	void Worker(HANDLE ready_event) noexcept;

	const std::wstring pipe_name_;
	px4::PipeServer::PipeServerConfig pipe_config_;
	px4::ReceiverManager &receiver_manager_;

	std::error_condition error_;
	mutable std::mutex mtx_;
	std::condition_variable cond_;
	std::deque<std::unique_ptr<Connection>> conns_;
	HANDLE quit_event_;
	std::unique_ptr<std::thread> th_;
};

} // namespace px4
