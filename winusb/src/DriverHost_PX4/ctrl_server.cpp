// ctrl_server.cpp

#include "ctrl_server.hpp"

namespace px4 {

CtrlServer::CtrlServer(px4::ReceiverManager &receiver_manager)
	: ServerBase(L"px4_ctrl_pipe", receiver_manager)
{
	pipe_config_.in_buffer_size = 512;
	pipe_config_.out_buffer_size = 512;
	pipe_config_.stream_pipe = false;
	pipe_config_.stream_read = false;
	pipe_config_.default_timeout = 2000;
}

px4::ServerBase::Connection* CtrlServer::CreateConnection(std::unique_ptr<px4::PipeServer> &pipe)
{
	return new CtrlConnection(*this, pipe);
}

CtrlServer::CtrlConnection::CtrlConnection(ServerBase &parent, std::unique_ptr<px4::PipeServer> &pipe) noexcept
	: Connection(parent, pipe)
{

}

void CtrlServer::CtrlConnection::Worker() noexcept
{
	std::size_t size = config_.in_buffer_size;
	std::unique_ptr<std::uint8_t[]> buf(new std::uint8_t[size]);
	px4::command::ReceiverInfo info = { 0 };
	px4::ReceiverBase *receiver = nullptr;

	while (true) {
		bool ret = true;
		std::size_t read;

		if (!conn_->Read(buf.get(), size, read, quit_event_))
			break;

		px4::command::CtrlCmdHeader *hdr = reinterpret_cast<px4::command::CtrlCmdHeader *>(buf.get());

		switch (hdr->cmd) {
		case px4::command::CtrlCmdCode::GET_VERSION:
		{
			px4::command::CtrlVersionCmd *version = reinterpret_cast<px4::command::CtrlVersionCmd *>(buf.get());

			version->status = px4::command::CtrlStatusCode::SUCCEEDED;
			version->driver_version = 0x00040000;
			version->cmd_version = px4::command::VERSION;

			break;
		}

		case px4::command::CtrlCmdCode::OPEN:
		{
			if (receiver) {
				receiver->Close();
				receiver_manager_.ClearDataId(receiver);
				receiver = nullptr;
			}

			px4::command::CtrlOpenCmd *open = reinterpret_cast<px4::command::CtrlOpenCmd *>(buf.get());
			std::uint32_t data_id;

			receiver = receiver_manager_.SearchAndOpen(open->receiver_info, info, data_id);
			if (receiver) {
				open->receiver_info = info;
				open->receiver_info.data_id = data_id;
			}

			open->status = (receiver) ? px4::command::CtrlStatusCode::SUCCEEDED : px4::command::CtrlStatusCode::FAILED;
			break;
		}

		case px4::command::CtrlCmdCode::CLOSE:
			if (receiver) {
				receiver->Close();
				receiver_manager_.ClearDataId(receiver);
				receiver = nullptr;
				info = { 0 };

				hdr->status = px4::command::CtrlStatusCode::SUCCEEDED;
			} else {
				hdr->status = px4::command::CtrlStatusCode::FAILED;
			}

			break;

		case px4::command::CtrlCmdCode::GET_INFO:
		{
			px4::command::CtrlReceiverInfoCmd *receiver_info = reinterpret_cast<px4::command::CtrlReceiverInfoCmd *>(buf.get());

			if (receiver) {
				receiver_info->receiver_info = info;
				hdr->status = px4::command::CtrlStatusCode::SUCCEEDED;
			} else {
				hdr->status = px4::command::CtrlStatusCode::FAILED;
			}

			break;
		}

		case px4::command::CtrlCmdCode::SET_CAPTURE:
		{
			px4::command::CtrlCaptureCmd *capture = reinterpret_cast<px4::command::CtrlCaptureCmd *>(buf.get());

			if (receiver && !receiver->SetCapture((capture->capture) ? true : false))
				capture->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				capture->status = px4::command::CtrlStatusCode::FAILED;

			break;
		}

		case px4::command::CtrlCmdCode::GET_PARAMS:
		{
			px4::command::CtrlParamsCmd *params = reinterpret_cast<px4::command::CtrlParamsCmd *>(buf.get());

			if (receiver && receiver->GetParameters(params->param_set))
				params->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				params->status = px4::command::CtrlStatusCode::FAILED;

			break;
		}

		case px4::command::CtrlCmdCode::SET_PARAMS:
		{
			px4::command::CtrlParamsCmd *params = reinterpret_cast<px4::command::CtrlParamsCmd *>(buf.get());

			if (receiver && receiver->SetParameters(params->param_set))
				params->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				params->status = px4::command::CtrlStatusCode::FAILED;

			break;
		}

		case px4::command::CtrlCmdCode::CLEAR_PARAMS:
			if (receiver && (receiver->ClearParameters(), true))
				hdr->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				hdr->status = px4::command::CtrlStatusCode::FAILED;

			break;

		case px4::command::CtrlCmdCode::TUNE:
		{
			px4::command::CtrlTuneCmd *tune = reinterpret_cast<px4::command::CtrlTuneCmd *>(buf.get());

			if (receiver && receiver->Tune(tune->timeout))
				hdr->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				hdr->status = px4::command::CtrlStatusCode::FAILED;

			break;
		}

		case px4::command::CtrlCmdCode::CHECK_LOCK:
		{
			px4::command::CtrlCheckLockCmd *check_lock = reinterpret_cast<px4::command::CtrlCheckLockCmd *>(buf.get());

			if (receiver && !receiver->CheckLock(check_lock->locked))
				check_lock->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				check_lock->status = px4::command::CtrlStatusCode::FAILED;

			break;
		}

		case px4::command::CtrlCmdCode::SET_LNB_VOLTAGE:
		{
			px4::command::CtrlLnbVoltageCmd *lnb = reinterpret_cast<px4::command::CtrlLnbVoltageCmd *>(buf.get());

			if (receiver && !receiver->SetLnbVoltage(lnb->voltage))
				lnb->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				lnb->status = px4::command::CtrlStatusCode::FAILED;

			break;
		}

		case px4::command::CtrlCmdCode::READ_STATS:
		{
			px4::command::CtrlStatsCmd *stats = reinterpret_cast<px4::command::CtrlStatsCmd *>(buf.get());

			if (receiver && receiver->ReadStats(stats->stat_set))
				stats->status = px4::command::CtrlStatusCode::SUCCEEDED;
			else
				stats->status = px4::command::CtrlStatusCode::FAILED;

			break;
		}

		default:
			hdr->status = px4::command::CtrlStatusCode::FAILED;
			break;
		}

		if (!ret)
			break;

		std::size_t written;

		if (!conn_->Write(buf.get(), read, written) || read != written)
			break;
	}

	if (receiver) {
		receiver->Close();
		receiver_manager_.ClearDataId(receiver);
	}

	delete this;
}

} // namespace px4
