// cmd_client.cpp

#include "cmd_client.hpp"

namespace px4 {

bool CtrlCmdClient::Open(const px4::command::ReceiverInfo &in, px4::SystemType systems, px4::command::ReceiverInfo *out) noexcept
{
	px4::command::CtrlOpenCmd open_cmd;

	if ((systems & in.systems) == px4::SystemType::UNSPECIFIED)
		return false;

	open_cmd.cmd = px4::command::CtrlCmdCode::OPEN;
	open_cmd.status = px4::command::CtrlStatusCode::NONE;
	wcscpy_s(open_cmd.receiver_info.device_name, in.device_name);
	open_cmd.receiver_info.device_guid = in.device_guid;
	wcscpy_s(open_cmd.receiver_info.receiver_name, in.receiver_name);
	open_cmd.receiver_info.receiver_guid = in.receiver_guid;
	open_cmd.receiver_info.systems = systems & in.systems;
	open_cmd.receiver_info.index = in.index;
	open_cmd.receiver_info.data_id = 0;

	bool ret = Call(open_cmd);

	if (out)
		*out = open_cmd.receiver_info;

	return ret;
}

bool CtrlCmdClient::Close() noexcept
{
	px4::command::CtrlCloseCmd close_cmd;

	close_cmd.cmd = px4::command::CtrlCmdCode::CLOSE;
	close_cmd.status = px4::command::CtrlStatusCode::NONE;

	return Call(close_cmd);
}

bool CtrlCmdClient::GetInfo(px4::command::ReceiverInfo &receiver_info) noexcept
{
	px4::command::CtrlReceiverInfoCmd info_cmd;

	info_cmd.cmd = px4::command::CtrlCmdCode::GET_INFO;
	info_cmd.status = px4::command::CtrlStatusCode::NONE;
	info_cmd.receiver_info = receiver_info;

	bool ret = Call(info_cmd);

	if (ret)
		receiver_info = info_cmd.receiver_info;

	return ret;
}

bool CtrlCmdClient::SetCapture(bool capture) noexcept
{
	px4::command::CtrlCaptureCmd capture_cmd;

	capture_cmd.cmd = px4::command::CtrlCmdCode::SET_CAPTURE;
	capture_cmd.status = px4::command::CtrlStatusCode::NONE;
	capture_cmd.capture = capture;

	return Call(capture_cmd);
}

bool CtrlCmdClient::GetParams(px4::command::ParameterSet &param_set) noexcept
{
	px4::command::CtrlParamsCmd params_cmd;

	params_cmd.cmd = px4::command::CtrlCmdCode::GET_PARAMS;
	params_cmd.status = px4::command::CtrlStatusCode::NONE;
	params_cmd.param_set = param_set;

	bool ret = Call(params_cmd);

	if (ret)
		param_set = params_cmd.param_set;

	return ret;
}

bool CtrlCmdClient::SetParams(const px4::command::ParameterSet &param_set) noexcept
{
	px4::command::CtrlParamsCmd params_cmd;

	params_cmd.cmd = px4::command::CtrlCmdCode::SET_PARAMS;
	params_cmd.status = px4::command::CtrlStatusCode::NONE;
	params_cmd.param_set = param_set;

	return Call(params_cmd);
}

bool CtrlCmdClient::ClearParams() noexcept
{
	px4::command::CtrlClearParamsCmd clear_cmd;

	clear_cmd.cmd = px4::command::CtrlCmdCode::CLEAR_PARAMS;
	clear_cmd.status = px4::command::CtrlStatusCode::NONE;

	return Call(clear_cmd);
}

bool CtrlCmdClient::Tune(std::uint32_t timeout) noexcept
{
	px4::command::CtrlTuneCmd tune_cmd;

	tune_cmd.cmd = px4::command::CtrlCmdCode::TUNE;
	tune_cmd.status = px4::command::CtrlStatusCode::NONE;
	tune_cmd.timeout = timeout;

	return Call(tune_cmd);
}

bool CtrlCmdClient::CheckLock(bool &locked) noexcept
{
	px4::command::CtrlCheckLockCmd check_lock_cmd;

	check_lock_cmd.cmd = px4::command::CtrlCmdCode::CHECK_LOCK;
	check_lock_cmd.status = px4::command::CtrlStatusCode::NONE;
	check_lock_cmd.locked = false;

	bool ret = Call(check_lock_cmd);

	if (ret)
		locked = check_lock_cmd.locked;

	return ret;
}

bool CtrlCmdClient::SetLnbVoltage(std::int32_t voltage) noexcept
{
	px4::command::CtrlLnbVoltageCmd lnb_cmd;

	lnb_cmd.cmd = px4::command::CtrlCmdCode::SET_LNB_VOLTAGE;
	lnb_cmd.status = px4::command::CtrlStatusCode::NONE;
	lnb_cmd.voltage = voltage;

	return Call(lnb_cmd);
}

bool CtrlCmdClient::ReadStats(px4::command::StatSet &stat_set) noexcept
{
	px4::command::CtrlStatsCmd stat_cmd;

	stat_cmd.cmd = px4::command::CtrlCmdCode::READ_STATS;
	stat_cmd.status = px4::command::CtrlStatusCode::NONE;
	stat_cmd.stat_set = stat_set;

	bool ret = Call(stat_cmd);

	if (ret)
		stat_set = stat_cmd.stat_set;

	return ret;
}

template <typename T>
bool CtrlCmdClient::Call(T& cmd) noexcept
{
	if (!pipe_)
		return false;

	return (pipe_->Call(&cmd, sizeof(cmd)) && cmd.status == px4::command::CtrlStatusCode::SUCCEEDED);
}

} // namespace px4
