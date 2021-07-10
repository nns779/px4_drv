// cmd_client.hpp

#pragma once

#include <memory>

#include "command.hpp"
#include "pipe.hpp"
#include "pipe_client.hpp"

namespace px4 {

class CtrlCmdClient final {
public:
	explicit CtrlCmdClient(px4::PipeClient *pipe = nullptr) noexcept : pipe_(pipe) {}
	~CtrlCmdClient() {}

	// cannot copy
	CtrlCmdClient(const CtrlCmdClient &) = delete;
	CtrlCmdClient& operator=(const CtrlCmdClient &) = delete;

	// cannot move
	CtrlCmdClient(CtrlCmdClient &&) = delete;
	CtrlCmdClient& operator=(CtrlCmdClient &&) = delete;

	void SetPipe(px4::PipeClient *pipe) noexcept { pipe_.reset(pipe); }
	void SetPipe(std::unique_ptr<px4::PipeClient> &pipe) noexcept { pipe_ = std::move(pipe); }
	void ClearPipe() noexcept { pipe_.reset(); }

	bool Open(const px4::command::ReceiverInfo &in, px4::SystemType systems, px4::command::ReceiverInfo *out) noexcept;
	bool Close() noexcept;
	bool GetInfo(px4::command::ReceiverInfo &receiver_info) noexcept;
	bool SetCapture(bool capture) noexcept;
	bool GetParams(px4::command::ParameterSet &param_set) noexcept;
	bool SetParams(const px4::command::ParameterSet &param_set) noexcept;
	bool ClearParams() noexcept;
	bool Tune(std::uint32_t timeout) noexcept;
	bool CheckLock(bool &locked) noexcept;
	bool SetLnbVoltage(std::int32_t voltage) noexcept;
	bool ReadStats(px4::command::StatSet &stat_set) noexcept;

private:
	template <typename T> bool Call(T &cmd) noexcept;

	std::unique_ptr<px4::PipeClient> pipe_;
};

} // namespace px4
