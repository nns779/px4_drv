// notify_icon.cpp

#include "notify_icon.hpp"

#include <tchar.h>

namespace px4 {

#define WM_NOTIFYICON (WM_USER + 0x100);

UINT NotifyIcon::next_id_ = 1;

NotifyIcon::NotifyIcon(LPCWSTR icon_name, std::wstring tip)
	: hinst_(GetModuleHandleW(nullptr)),
	hwnd_(nullptr),
	param_(new CreateParam(icon_name, tip)),
	mtx_(),
	cond_(),
	ready_(false),
	th_(&px4::NotifyIcon::Worker, this)
{
	std::unique_lock<std::mutex> lock(mtx_);

	cond_.wait(lock);
	param_.release();

	if (!ready_.load()) {
		lock.unlock();
		throw NotifyIconError("px4::NotifyIcon::NotifyIcon: failed.");
	}
}

NotifyIcon::~NotifyIcon()
{
	SendMessageW(hwnd_, WM_CLOSE, 0, 0);
	th_.join();
}

LRESULT CALLBACK NotifyIcon::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	void *p;
	NotifyIcon *ni = nullptr;
	static NOTIFYICONDATAW nid;

	p = reinterpret_cast<void *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (p) {
		ni = reinterpret_cast<NotifyIcon *>(DecodePointer(p));
	} else if (uMsg == WM_CREATE && lParam) {
		ni = reinterpret_cast<NotifyIcon *>(reinterpret_cast<CREATESTRUCT *>(lParam)->lpCreateParams);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(EncodePointer(ni)));
	}

	if (!ni)
		return -1;

	std::unique_lock<std::mutex> lock(ni->mtx_, std::defer_lock);

	switch (uMsg) {
	case WM_CREATE:
	{
		nid.cbSize = sizeof(NOTIFYICONDATAW);
		nid.hWnd = hwnd;
		nid.uID = next_id_++;
		nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
		nid.uCallbackMessage = WM_NOTIFYICON;
		nid.hIcon = LoadIconW(ni->hinst_, ni->param_->icon_name);
		wcscpy_s(nid.szTip, sizeof(nid.szTip) / sizeof(nid.szTip[0]), ni->param_->tip.c_str());

		Shell_NotifyIconW(NIM_ADD, &nid);

		return 0;
	}

	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;

	case WM_DESTROY:
		lock.lock();

		Shell_NotifyIcon(NIM_DELETE, &nid);
		DestroyIcon(nid.hIcon);

		lock.unlock();

		PostQuitMessage(0);
		return 0;

	default:
		break;
	}

	return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void NotifyIcon::Worker()
{
	WCHAR class_name[] = TEXT("NotifyIcon_MessageWindow");
	WNDCLASSEXW wc;
	MSG msg;

	wc.cbSize = sizeof(wc);
	wc.style = 0;
	wc.lpfnWndProc = WndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = hinst_;
	wc.hIcon = nullptr;
	wc.hCursor = nullptr;
	wc.hbrBackground = nullptr;
	wc.lpszMenuName = nullptr;
	wc.lpszClassName = class_name;
	wc.hIconSm = nullptr;

	if (!RegisterClassExW(&wc)) {
		ready_.store(false);
		cond_.notify_all();
		return;
	}

	hwnd_ = CreateWindowExW(0, class_name, nullptr, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, HWND_MESSAGE, nullptr, hinst_, this);
	if (!hwnd_) {
		ready_.store(false);
		cond_.notify_all();
		return;
	}

	ready_.store(true);
	cond_.notify_all();

	while (GetMessage(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	return;
}

} // namespace px4
