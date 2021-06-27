// device_notifier.hpp

#pragma once

#include <string>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <stdexcept>

#include <windows.h>
#include <dbt.h>

namespace px4 {

enum class DeviceNotifyType {
	UNDEFINED = 0,
	ARRIVAL,
	REMOVE,
};

class DeviceNotifyHandler {
public:
	virtual ~DeviceNotifyHandler() {}

	virtual void Handle(DeviceNotifyType type, const GUID &interface_guid, const wchar_t *path) noexcept = 0;
};

class DeviceNotifier final {
public:
	explicit DeviceNotifier(DeviceNotifyHandler *handler);
	~DeviceNotifier();

	// cannot copy
	DeviceNotifier(const DeviceNotifier &) = delete;
	DeviceNotifier& operator=(const DeviceNotifier &) = delete;

	// cannot move
	DeviceNotifier(DeviceNotifier &&) = delete;
	DeviceNotifier& operator=(DeviceNotifier &&) = delete;

private:
	static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	void Worker();

	DeviceNotifyHandler *handler_;
	HINSTANCE hinst_;
	HWND hwnd_;
	HDEVNOTIFY notify_handle_;
	std::mutex mtx_;
	std::condition_variable cond_;
	std::atomic_bool ready_;
	std::thread th_;
};

class DeviceNotifierError : public std::runtime_error {
public:
	explicit DeviceNotifierError(const std::string &what_arg) : runtime_error(what_arg.c_str()) {};
};

} // namespace px4
