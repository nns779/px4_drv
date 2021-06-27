// notify_icon.hpp

#pragma once

#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <thread>
#include <stdexcept>

#include <windows.h>

namespace px4 {

class NotifyIcon final {
public:
	NotifyIcon(LPCWSTR icon_name, std::wstring tip);
	~NotifyIcon();

	// cannot copy
	NotifyIcon(const NotifyIcon &) = delete;
	NotifyIcon& operator=(const NotifyIcon &) = delete;

	// cannot move
	NotifyIcon(NotifyIcon &&) = delete;
	NotifyIcon& operator=(NotifyIcon &&) = delete;

private:
	static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	void Worker();

	struct CreateParam {
		CreateParam(LPCWSTR icon_name, std::wstring &tip)
			: icon_name(icon_name), tip(tip)
		{
		}

		LPCWSTR icon_name;
		std::wstring &tip;
	};

	static UINT next_id_;
	HINSTANCE hinst_;
	HWND hwnd_;
	std::unique_ptr<CreateParam> param_;
	std::mutex mtx_;
	std::condition_variable cond_;
	std::atomic_bool ready_;
	std::thread th_;
};

class NotifyIconError : public std::runtime_error {
public:
	explicit NotifyIconError(const std::string &what_arg) : runtime_error(what_arg.c_str()) {};
};

} // namespace px4
