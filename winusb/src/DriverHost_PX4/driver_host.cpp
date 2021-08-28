// driver_host.cpp

#include "driver_host.hpp"

#include <aclapi.h>

#include "security_attributes.hpp"
#include "notify_icon.hpp"

namespace px4 {

DriverHost::DriverHost()
	: mutex_(nullptr),
	startup_event_(nullptr)
{
	configs_.Load(px4::util::path::GetFileBase() + L".ini");

	dev_defs_.Load(configs_);
}

DriverHost::~DriverHost()
{
	ctrl_server_.reset();
	device_manager_.reset();

	if (startup_event_)
		CloseHandle(startup_event_);

	if (mutex_) {
		ReleaseMutex(mutex_);
		CloseHandle(mutex_);
	}
}

void DriverHost::Run()
{
	{
		SecurityAttributes sa(MUTEX_ALL_ACCESS);
		mutex_ = CreateMutexW(sa.Get(), TRUE, L"Global\\DriverHost_PX4_Mutex");
	}

	if (!mutex_)
		throw DriverHostError("px4::DriverHost::Run: CreateMutexW() failed.");

	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(mutex_);
		mutex_ = nullptr;
		return;
	}

	{
		SecurityAttributes sa(EVENT_ALL_ACCESS);
		startup_event_ = CreateEventW(sa.Get(), TRUE, FALSE, L"Global\\DriverHost_PX4_StartupEvent");
	}

	if (!startup_event_) {
		CloseHandle(mutex_);
		throw DriverHostError("px4::DriverHost::Run: CreateEventW() failed.");
	}

	device_manager_.reset(new px4::DeviceManager(dev_defs_, receiver_manager_));

	ctrl_server_.reset(new px4::CtrlServer(receiver_manager_));
	stream_server_.reset(new px4::StreamServer(receiver_manager_));

	ctrl_server_->Start();
	stream_server_->Start();

	SetEvent(startup_event_);

	{
		NotifyIcon ni(L"ICON1", L"PX4 Device Driver");
		int n = 0;

		while (n < 3) {
			Sleep(5000);
			if (!ctrl_server_->GetActiveConnectionCount() && !stream_server_->GetActiveConnectionCount())
				n++;
			else
				n = 0;
		}
	}

	stream_server_.reset();
	ctrl_server_.reset();
	device_manager_.reset();

	CloseHandle(startup_event_);
	startup_event_ = nullptr;

	ReleaseMutex(mutex_);
	CloseHandle(mutex_);
	mutex_ = nullptr;
}

} // namespace px4
