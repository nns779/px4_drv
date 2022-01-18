// bon_driver.cpp

#include <string>
#include <map>
#include <stdexcept>

#include <windows.h>
#include <aclapi.h>
#include <shlwapi.h>

#include "bon_driver.hpp"
#include "config.hpp"
#include "chset.hpp"
#include "security_attributes.hpp"
#include "pipe_client.hpp"
#include "command.hpp"
#include "util.hpp"

namespace px4 {

BonDriver::BonDriver() noexcept
	: mtx_(),
	driver_host_path_(),
	configs_(),
	name_(),
	systems_(px4::SystemType::UNSPECIFIED),
	chset_(),
	receivers_(),
	lnb_power_(false),
	lnb_power_state_(false),
	pipe_timeout_(2000),
	tune_timeout_(5000),
	ctrl_client_(nullptr),
	data_pipe_(nullptr),
	open_(FALSE),
	space_(0),
	ch_(0),
	display_error_message_(false),
	stream_mtx_(),
	ioq_(nullptr),
	iorp_(*this),
	current_ofs_(0),
	quit_event_(nullptr)
{
	
}

BonDriver::~BonDriver()
{
	CloseTuner();
	Term();
}

bool BonDriver::Init()
{
	try {
		const std::wstring &dir_path = px4::util::path::GetDir();

		if (!configs_.Load(px4::util::path::GetFileBase() + L".ini"))
			return false;

		std::size_t num_packets = 1024;
		std::uintptr_t max_buffers = 64, min_buffers = 4;
		int data_ignore_count = 1;

		if (configs_.Exists(L"BonDriver")) {
			const px4::Config &bon_config = configs_.Get(L"BonDriver");
			const std::wstring &mode = bon_config.Get(L"System", L"ISDB-T");
			WCHAR path[MAX_PATH];

			if (!px4::util::ParseSystemStr(mode, systems_))
				return false;

			driver_host_path_ = bon_config.Get(L"DriverHostPath", dir_path + L"DriverHost_PX4.exe");
			if (PathIsRelativeW(driver_host_path_.c_str()) && PathCanonicalizeW(path, (dir_path + driver_host_path_).c_str()))
				driver_host_path_ = path;

			name_ = bon_config.Get(L"Name", L"PX4");

			try {
				pipe_timeout_ = px4::util::wtoui32(bon_config.Get(L"PipeConnectTimeout"));
			} catch (const std::out_of_range &) {}

			try {
				tune_timeout_ = px4::util::wtoui32(bon_config.Get(L"TuneTimeout"));
			} catch (const std::out_of_range &) {}

			try {
				num_packets = px4::util::wtoui(bon_config.Get(L"NumberOfPacketsPerBuffer"));
				if (num_packets <= 0)
					return false;
			} catch (const std::out_of_range &) {}

			try {
				max_buffers = px4::util::wtoui(bon_config.Get(L"MaximumNumberOfBuffers"));
				if (max_buffers <= 0)
					return false;
			} catch (const std::out_of_range &) {}

			try {
				min_buffers = px4::util::wtoui(bon_config.Get(L"MinimumNumberOfBuffers"));
				if (min_buffers <= 0)
					return false;
			} catch (const std::out_of_range &) {}

			if (max_buffers < min_buffers)
				return false;

			try {
				data_ignore_count = px4::util::wtoi(bon_config.Get(L"NumberOfBuffersToIgnoreAfterPurge"));
				if (data_ignore_count < 0)
					return false;
			} catch (const std::out_of_range &) {}

			try {
				int display_error_message = px4::util::wtoi(bon_config.Get(L"DisplayErrorMessage"));
				if (display_error_message == 1) {
					display_error_message_ = true;
				}
			} catch (const std::out_of_range &) {}
		} else {
			systems_ = px4::SystemType::ISDB_T;
			driver_host_path_ = dir_path + L"DriverHost_PX4.exe";
			name_ = L"PX4";
		}

		if ((systems_ & px4::SystemType::ISDB_T) == px4::SystemType::ISDB_T) {
			px4::ChannelSet chset_t;
			bool result;

			try {
				std::wstring chset_path = configs_.Get(L"BonDriver.ISDB-T").Get(L"ChSetPath");
				WCHAR path[MAX_PATH];

				if (PathIsRelativeW(chset_path.c_str()) && PathCanonicalizeW(path, (dir_path + chset_path).c_str()))
					chset_path = path;

				result = chset_t.Load(chset_path, px4::SystemType::ISDB_T);
			} catch (const std::out_of_range&) {
				result = false;
			}

			if (!result) {
				chset_t.Clear();
				result = chset_t.Load(dir_path + L"BonDriver_PX4-T.ChSet.txt", px4::SystemType::ISDB_T);
			}

			if (result)
				chset_.Merge(chset_t);
		}

		if ((systems_ & px4::SystemType::ISDB_S) == px4::SystemType::ISDB_S) {
			px4::ChannelSet chset_s;
			bool result;

			try {
				auto &config = configs_.Get(L"BonDriver.ISDB-S");

				try {
					lnb_power_ = !!px4::util::wtoi(config.Get(L"LNBPower"));
				} catch (const std::out_of_range &) {}

				std::wstring chset_path = config.Get(L"ChSetPath");
				WCHAR path[MAX_PATH];

				if (PathIsRelativeW(chset_path.c_str()) && PathCanonicalizeW(path, (dir_path + chset_path).c_str()))
					chset_path = path;

				result = chset_s.Load(chset_path, px4::SystemType::ISDB_S);
			} catch (const std::out_of_range&) {
				result = false;
			}

			if (!result) {
				chset_s.Clear();
				result = chset_s.Load(dir_path + L"BonDriver_PX4-S.ChSet.txt", px4::SystemType::ISDB_S);
			}

			if (result)
				chset_.Merge(chset_s);
		}

		receivers_.Load(configs_);

		quit_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!quit_event_)
			return false;

		ioq_.reset(new px4::IoQueue(px4::IoQueue::IoOperation::READ, iorp_, 188 * num_packets, max_buffers, min_buffers, data_ignore_count));
	} catch (const std::runtime_error &e) {
		if (display_error_message_) MessageBoxA(nullptr, e.what(), "BonDriver_PX4 (BonDriver::Init)", MB_OK);
		return false;
	} catch (...) {
		if (display_error_message_) MessageBoxA(nullptr, "Fatal error!", "BonDriver_PX4 (BonDriver::Init)", MB_OK);
		return false;
	}

	return true;
}

void BonDriver::Term() noexcept
{
	ioq_.reset();

	if (quit_event_)
		CloseHandle(quit_event_);
}

const BOOL BonDriver::OpenTuner()
{
	BOOL ret = TRUE;
	std::lock_guard<std::mutex> lock(mtx_);

	if (open_)
		return TRUE;

	try {
		HANDLE startup_event;
		DWORD st;

		{
			SecurityAttributes sa(EVENT_ALL_ACCESS);
			startup_event = CreateEventW(sa.Get(), TRUE, FALSE, L"Global\\DriverHost_PX4_StartupEvent");
		}

		if (!startup_event) {
			throw BonDriverError("BonDriver::OpenTuner: CreateEventW(\"DriverHost_PX4_StartupEvent\") failed.");
		} else if (GetLastError() != ERROR_ALREADY_EXISTS) {
			STARTUPINFO si;
			PROCESS_INFORMATION pi;

			memset(&si, 0, sizeof(si));
			si.cb = sizeof(si);
			memset(&pi, 0, sizeof(pi));

			if (!CreateProcessW(driver_host_path_.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
				CloseHandle(startup_event);
				throw BonDriverError("BonDriver::OpenTuner: CreateProcessW() failed.");
			}

			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}

		st = WaitForSingleObject(startup_event, 10000);
		CloseHandle(startup_event);

		if (st != WAIT_OBJECT_0)
			throw BonDriverError("BonDriver::OpenTuner: WaitForSingleObject() failed.");

		std::unique_ptr<px4::PipeClient> ctrl_pipe(new px4::PipeClient());
		px4::PipeClient::PipeClientConfig ctrl_pipe_config;
		px4::command::ReceiverInfo ri_res;

		ctrl_pipe_config.stream_read = false;
		ctrl_pipe_config.timeout = pipe_timeout_;

		if (!ctrl_pipe->Connect(L"px4_ctrl_pipe", ctrl_pipe_config, nullptr))
			throw BonDriverError("BonDriver::OpenTuner: control pipe: cannot connect.");

		ctrl_client_.SetPipe(ctrl_pipe);

		try {
			for (std::size_t i = 0; ; i++) {
				const px4::command::ReceiverInfo &ri = receivers_.Get(i);

				if (ctrl_client_.Open(ri, systems_, &ri_res))
					break;
			}
		} catch (const std::out_of_range &) {
			throw BonDriverError("BonDriver::OpenTuner: no receivers available.");
		}

		px4::PipeClient::PipeClientConfig data_pipe_config;

		data_pipe_config.stream_read = true;
		data_pipe_config.timeout = pipe_timeout_;

		data_pipe_.reset(new px4::PipeClient());

		if (!data_pipe_->Connect(L"px4_data_pipe", data_pipe_config, nullptr))
			throw BonDriverError("BonDriver::OpenTuner: data pipe: cannot connect.");

		px4::command::DataCmd data_cmd;
		std::size_t ret_size;

		data_cmd.cmd = px4::command::DataCmdCode::SET_DATA_ID;
		data_cmd.data_id = ri_res.data_id;

		if (!data_pipe_->Write(&data_cmd, sizeof(data_cmd), ret_size))
			throw BonDriverError("BonDriver::OpenTuner: command failed.");

		if (!ctrl_client_.SetCapture(true))
			throw BonDriverError("BonDriver::OpenTuner: command failed.");

		ioq_->Start();

		open_ = TRUE;
	} catch (const std::exception &e) {
		ret = FALSE;
		if (display_error_message_) MessageBoxA(nullptr, e.what(), "BonDriver_PX4 (BonDriver::OpenTuner)", MB_OK | MB_ICONERROR);
	} catch (...) {
		ret = FALSE;
		if (display_error_message_) MessageBoxA(nullptr, "Fatal error!", "BonDriver_PX4 (BonDriver::OpenTuner)", MB_OK | MB_ICONERROR);
	}

	if (!open_) {
		if (data_pipe_) {
			SetEvent(quit_event_);
			ioq_->Stop();
			data_pipe_.reset();
		}

		ctrl_client_.Close();
		ctrl_client_.ClearPipe();
	}

	return ret;
}

void BonDriver::CloseTuner()
{
	std::lock_guard<std::mutex> lock(mtx_);

	if (!open_)
		return;

	if (data_pipe_) {
		SetEvent(quit_event_);
		ioq_->Stop();
		data_pipe_.reset();
	}

	if (lnb_power_state_) {
		ctrl_client_.SetLnbVoltage(0);
		lnb_power_state_ = false;
	}

	ctrl_client_.Close();
	ctrl_client_.ClearPipe();

	space_ = 0;
	ch_ = 0;

	open_ = FALSE;

	return;
}

const BOOL BonDriver::SetChannel(const BYTE bCh)
{
	return SetChannel(0, bCh);
}

const float BonDriver::GetSignalLevel(void)
{
	std::lock_guard<std::mutex> lock(mtx_);

	if (!open_)
		return 0.0f;

	px4::command::StatSet stat_set;

	stat_set.num = 1;
	stat_set.data[0].type = px4::command::StatType::CNR;
	stat_set.data[0].value = 0;

	if (!ctrl_client_.ReadStats(stat_set))
		return 0.0f;

	return static_cast<float>(stat_set.data[0].value) / 1000.0f;
}

const DWORD BonDriver::WaitTsStream(const DWORD dwTimeOut)
{
	return (ioq_->WaitDataBuffer(std::chrono::milliseconds((dwTimeOut == INFINITE) ? 5000 : dwTimeOut))) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

const DWORD BonDriver::GetReadyCount(void)
{
	return static_cast<::DWORD>(ioq_->GetDataBufferCount() + (ioq_->HaveReadingBuffer() ? 1 : 0));
}

const BOOL BonDriver::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	if (!pDst || !pdwSize)
		return FALSE;

	std::size_t size, remain;

	if (!ioq_->Read(pDst, size, remain, false))
		return FALSE;

	*pdwSize = static_cast<::DWORD>(size);
	if (pdwRemain)
		*pdwRemain = static_cast<::DWORD>(remain);

	return TRUE;
}

const BOOL BonDriver::GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	if (!ppDst || !pdwSize)
		return FALSE;

	std::size_t size, remain;

	if (!ioq_->ReadBuffer(reinterpret_cast<void **>(ppDst), size, remain, false))
		return FALSE;

	*pdwSize = static_cast<::DWORD>(size);
	if (pdwRemain)
		*pdwRemain = static_cast<::DWORD>(remain);

	return TRUE;
}

void BonDriver::PurgeTsStream(void)
{
	{
		px4::command::DataCmd data_cmd;
		std::size_t ret_size;
		std::lock_guard<std::mutex> lock(mtx_);

		data_cmd.cmd = px4::command::DataCmdCode::PURGE;
		data_pipe_->Write(&data_cmd, sizeof(data_cmd), ret_size);
	}

	ioq_->PurgeDataBuffer();
	return;
}

void BonDriver::Release(void)
{
	delete this;
}

LPCTSTR BonDriver::GetTunerName(void)
{
	return name_.c_str();
}

const BOOL BonDriver::IsTunerOpening(void)
{
	return open_;
}

LPCTSTR BonDriver::EnumTuningSpace(const DWORD dwSpace)
{
	try {
		return chset_.GetSpaceName(dwSpace).c_str();
	} catch (const std::out_of_range&) {
		return nullptr;
	}
}

LPCTSTR BonDriver::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	try {
		return chset_.GetChannel(dwSpace, dwChannel).name.c_str();
	} catch (const std::out_of_range&) {
		return nullptr;
	}
}

const BOOL BonDriver::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	if (!open_)
		return FALSE;

	if (!chset_.ExistsChannel(dwSpace, dwChannel))
		return FALSE;

	const px4::ChannelSet::ChannelInfo &channel = chset_.GetChannel(dwSpace, dwChannel);
	px4::SystemType system = chset_.GetSpaceSystem(dwSpace);
	std::uint32_t real_freq, num_param = 0;

	switch (system) {
	case px4::SystemType::ISDB_T:
		if ((channel.ptx_ch >= 3 && channel.ptx_ch <= 12) || (channel.ptx_ch >= 22 && channel.ptx_ch <= 62)) {
			// CATV C13-22ch, C23-63ch
			real_freq = 93143 + (channel.ptx_ch * 6000);

			if (channel.ptx_ch == 12)
				real_freq += 2000;
		} else if (channel.ptx_ch >= 63 && channel.ptx_ch <= 112) {
			// UHF 13-62ch
			real_freq = 95143 + (channel.ptx_ch * 6000);
		} else {
			// unknown channel
			return FALSE;
		}

		break;

	case px4::SystemType::ISDB_S:
		if (channel.ptx_ch <= 11) {
			// BS
			real_freq = 1049480 + (channel.ptx_ch * 38360);
		} else if (channel.ptx_ch <= 23) {
			// CS 110
			real_freq = 1613000 + ((channel.ptx_ch - 12) * 40000);
		} else {
			// unknown channel
			return FALSE;
		}

		num_param = 1;

		break;

	default:
		// unknown system
		return FALSE;
	}

	px4::command::ParameterSet *param_set = reinterpret_cast<px4::command::ParameterSet*>(new std::uint8_t[sizeof(*param_set) - sizeof(param_set->params) + (sizeof(param_set->params[0]) * num_param)]);

	param_set->system = system;
	param_set->freq = real_freq;
	param_set->num = num_param;

	if (system == px4::SystemType::ISDB_S && num_param) {
		param_set->params[0].type = px4::command::ParameterType::STREAM_ID;
		param_set->params[0].value = channel.tsid;
	}

	bool ret = true;

	try {
		std::lock_guard<std::mutex> lock(mtx_);

		if (lnb_power_) {
			if (system == px4::SystemType::ISDB_S) {
				if (!lnb_power_state_) {
					ret = ctrl_client_.SetLnbVoltage(15);
					lnb_power_state_ = true;
				}
			} else if (lnb_power_state_) {
				ret = ctrl_client_.SetLnbVoltage(0);
				lnb_power_state_ = false;
			}
		}

		if (ret) {
			ret = ctrl_client_.SetParams(*param_set);
			if (ret)
				ret = ctrl_client_.Tune(tune_timeout_);
		}
	} catch (const std::exception &e) {
		if (display_error_message_) MessageBoxA(nullptr, e.what(), "BonDriver_PX4 (BonDriver::SetChannel)", MB_OK | MB_ICONERROR);
	} catch (...) {
		if (display_error_message_) MessageBoxA(nullptr, "Fatal error!", "BonDriver_PX4 (BonDriver::SetChannel)", MB_OK | MB_ICONERROR);
	}

	delete[] reinterpret_cast<std::uint8_t*>(param_set);

	PurgeTsStream();

	if (ret) {
		// succeeded
		space_ = dwSpace;
		ch_ = dwChannel;
	}

	return !!ret;
}

const DWORD BonDriver::GetCurSpace(void)
{
	return space_;
}

const DWORD BonDriver::GetCurChannel(void)
{
	return ch_;
}

bool BonDriver::ReadProvider::Start()
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	return true;
}

void BonDriver::ReadProvider::Stop()
{
	return;
}

bool BonDriver::ReadProvider::Do(void *buf, std::size_t &size)
{
	return parent_.data_pipe_->Read(buf, size, size, parent_.quit_event_);
}

} // namespace px4

#pragma warning(disable: 4273)
extern "C" IBonDriver* CreateBonDriver()
{
	try {
		auto bon = new px4::BonDriver();

		if (!bon)
			return nullptr;

		if (!bon->Init()) {
			delete bon;
			return nullptr;
		}

		return bon;
	} catch (const std::exception &e) {
		MessageBoxA(nullptr, e.what(), "BonDriver_PX4 (CreateBonDriver)", MB_ICONERROR | MB_OK);
		return nullptr;
	} catch (...) {
		MessageBoxA(nullptr, "Fatal error!", "BonDriver_PX4 (CreateBonDriver)", MB_ICONERROR | MB_OK);
		return nullptr;
	}
}
#pragma warning(default: 4273)
