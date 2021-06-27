// device_notifier.cpp

#include "device_notifier.hpp"

namespace px4 {

DeviceNotifier::DeviceNotifier(DeviceNotifyHandler *handler)
	: handler_(handler),
	hinst_(GetModuleHandleW(nullptr)),
	hwnd_(nullptr),
	notify_handle_(nullptr),
	mtx_(),
	cond_(),
	ready_(false),
	th_(&px4::DeviceNotifier::Worker, this)
{
	std::unique_lock<std::mutex> lock(mtx_);

	cond_.wait(lock);

	if (!ready_.load()) {
		lock.unlock();
		throw DeviceNotifierError("px4::DeviceNotifier::DeviceNotifier: failed.");
	}
}

DeviceNotifier::~DeviceNotifier()
{
	SendMessageW(hwnd_, WM_CLOSE, 0, 0);
	th_.join();
}

LRESULT CALLBACK DeviceNotifier::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	void *p;
	DeviceNotifier *dn = nullptr;

	p = reinterpret_cast<void *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (p) {
		dn = reinterpret_cast<DeviceNotifier *>(DecodePointer(p));
	} else if (uMsg == WM_CREATE && lParam) {
		dn = reinterpret_cast<DeviceNotifier *>(reinterpret_cast<CREATESTRUCT *>(lParam)->lpCreateParams);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(EncodePointer(dn)));
	}

	if (!dn)
		return -1;

	std::unique_lock<std::mutex> lock(dn->mtx_, std::defer_lock);

	switch (uMsg) {
	case WM_CREATE:
	{
		DEV_BROADCAST_DEVICEINTERFACE_W dbdi;
		HDEVNOTIFY notify_handle;

		dbdi.dbcc_size = sizeof(dbdi);
		dbdi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;

		notify_handle = RegisterDeviceNotificationW(hwnd, &dbdi, DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
		if (!notify_handle)
			return -1;

		dn->notify_handle_ = notify_handle;
		
		return 0;
	}

	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;

	case WM_DESTROY:
		lock.lock();

		if (dn->notify_handle_) {
			UnregisterDeviceNotification(dn->notify_handle_);
			dn->notify_handle_ = nullptr;
		}

		lock.unlock();

		PostQuitMessage(0);
		return 0;

	case WM_DEVICECHANGE:
	{
		DEV_BROADCAST_DEVICEINTERFACE_W *dbdi = reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W *>(lParam);

		if (!dbdi || dbdi->dbcc_devicetype != DBT_DEVTYP_DEVICEINTERFACE)
			return TRUE;

		lock.lock();

		switch (wParam) {
		case DBT_DEVICEARRIVAL:
			dn->handler_->Handle(DeviceNotifyType::ARRIVAL, dbdi->dbcc_classguid, dbdi->dbcc_name);
			break;

		case DBT_DEVICEREMOVECOMPLETE:
			dn->handler_->Handle(DeviceNotifyType::REMOVE, dbdi->dbcc_classguid, dbdi->dbcc_name);
			break;

		default:
			break;
		}

		lock.unlock();

		return TRUE;
	}

	default:
		break;
	}

	return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void DeviceNotifier::Worker()
{
	WCHAR class_name[] = TEXT("DeviceNotifier_MessageWindow");
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
