// steram_server.cpp

#define msg_prefix	"px4_winusb"

#include "stream_server.hpp"

#include <thread>

#include <windows.h>

#include "msg.h"
#include "command.hpp"

namespace px4 {

StreamServer::StreamServer(px4::ReceiverManager &receiver_manager) noexcept
	: ServerBase(L"px4_data_pipe", receiver_manager)
{
	pipe_config_.in_buffer_size = 512;
	pipe_config_.out_buffer_size = 188 * 4096;
	pipe_config_.stream_pipe = false;
	pipe_config_.stream_read = false;
	pipe_config_.default_timeout = 2000;
}

px4::ServerBase::Connection* StreamServer::CreateConnection(std::unique_ptr<px4::PipeServer> &pipe)
{
	return new StreamConnection(*this, pipe);
}

StreamServer::StreamConnection::StreamConnection(ServerBase &parent, std::unique_ptr<px4::PipeServer> &pipe) noexcept
	: Connection(parent, pipe)
{

}

void StreamServer::StreamConnection::Worker() noexcept
{
	std::size_t size = config_.in_buffer_size;
	std::unique_ptr<std::uint8_t[]> buf(new std::uint8_t[size]);
	px4::ReceiverBase *receiver = nullptr;
	std::unique_ptr<std::thread> stream_th;

	while (true) {
		std::size_t read;

		if (!conn_->Read(buf.get(), size, read, quit_event_))
			break;

		int ret = true;
		px4::command::DataCmd *cmd = reinterpret_cast<px4::command::DataCmd *>(buf.get());

		switch (cmd->cmd) {
		case px4::command::DataCmdCode::SET_DATA_ID:
			if (receiver)
				break;

			receiver = receiver_manager_.SearchByDataId(cmd->data_id);
			if (!receiver) {
				ret = false;
				break;
			}

			try {
				stream_th.reset(new std::thread(&px4::StreamServer::StreamConnection::StreamWorker, this, receiver->GetStreamBuffer()));
			} catch (...) {
				ret = false;
				break;
			}

			break;

		case px4::command::DataCmdCode::PURGE:
			if (!receiver)
				break;

			receiver->GetStreamBuffer()->Purge();
			break;

		default:
			ret = false;
			break;
		}

		if (!ret)
			break;
	}

	if (receiver && stream_th) {
		receiver->GetStreamBuffer()->StopRequest();

		stream_th->join();
		stream_th.reset();
	}

	delete this;
}

void StreamServer::StreamConnection::StreamWorker(std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf) noexcept
{
	msg_dbg("px4::StreamServer::StreamConnection::StreamWorker\n");

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

	stream_buf->HandleRead(config_.out_buffer_size / 4,
		[this](const void *buf, std::size_t size) {
			return conn_->Write(buf, size, size, quit_event_);
		}
	);

	msg_dbg("px4::StreamServer::StreamConnection::StreamWorker: exit\n");

	return;
}

} // namespace px4
