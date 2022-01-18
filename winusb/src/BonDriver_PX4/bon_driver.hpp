// bon_driver.hpp

#pragma once

#include <memory>
#include <string>
#include <mutex>
#include <map>
#include <queue>
#include <stdexcept>

#include <windows.h>

#include "IBonDriver2.h"
#include "config.hpp"
#include "chset.hpp"
#include "receiver_info_set.hpp"
#include "pipe_client.hpp"
#include "command.hpp"
#include "cmd_client.hpp"
#include "io_queue.hpp"

namespace px4 {

class BonDriver final : public IBonDriver2 {
public:
	BonDriver() noexcept;
	~BonDriver();

	bool Init();
	void Term() noexcept;

	// IBonDriver
	const BOOL OpenTuner(void) override;
	void CloseTuner(void) override;

	const BOOL SetChannel(const BYTE bCh) override;
	const float GetSignalLevel(void) override;

	const DWORD WaitTsStream(const DWORD dwTimeOut = 0) override;
	const DWORD GetReadyCount(void) override;

	const BOOL GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain) override;
	const BOOL GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain) override;

	void PurgeTsStream(void) override;

	void Release(void) override;

	// IBonDriver2
	LPCTSTR GetTunerName(void) override;

	const BOOL IsTunerOpening(void) override;

	LPCTSTR EnumTuningSpace(const DWORD dwSpace) override;
	LPCTSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel) override;

	const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel) override;

	const DWORD GetCurSpace(void) override;
	const DWORD GetCurChannel(void) override;

private:
	class ReadProvider final : public IoQueue::IoProvider {
	public:
		explicit ReadProvider(BonDriver& parent) : parent_(parent) {}
		~ReadProvider() {}

		bool Start() override;
		void Stop() override;
		bool Do(void *buf, std::size_t &size) override;

	private:
		BonDriver &parent_;
	};

	std::mutex mtx_;
	std::wstring driver_host_path_;
	px4::ConfigSet configs_;
	std::wstring name_;
	px4::SystemType systems_;
	px4::ChannelSet chset_;
	px4::ReceiverInfoSet receivers_;
	bool lnb_power_;
	bool lnb_power_state_;
	std::uint32_t pipe_timeout_;
	std::uint32_t tune_timeout_;
	px4::CtrlCmdClient ctrl_client_;
	std::unique_ptr<px4::PipeClient> data_pipe_;
	BOOL open_;
	DWORD space_, ch_;
	BOOL display_error_message_;

	std::mutex stream_mtx_;
	std::unique_ptr<px4::IoQueue> ioq_;
	ReadProvider iorp_;
	std::size_t current_ofs_;
	HANDLE quit_event_;
};

class BonDriverError : public std::runtime_error {
public:
	explicit BonDriverError(const std::string &what_arg) : runtime_error(what_arg.c_str()) {};
};

} // namespace px4
