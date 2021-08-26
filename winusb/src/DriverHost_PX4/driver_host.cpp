// driver_host.cpp

#include "driver_host.hpp"

#include <aclapi.h>

#include "notify_icon.hpp"

namespace px4 {

DriverHost::DriverHost()
	: startup_event_(nullptr)
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
}

void DriverHost::Run()
{
	SID_IDENTIFIER_AUTHORITY sia;
	PSID sid = nullptr;
	EXPLICIT_ACCESSW ea;
	PACL acl = nullptr;
	SECURITY_DESCRIPTOR sd;
	SECURITY_ATTRIBUTES sa;

	sia = SECURITY_WORLD_SID_AUTHORITY;

	if (!AllocateAndInitializeSid(&sia, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &sid))
		throw DriverHostError("px4::DriverHost::Run: AllocateAndInitializeSid() failed.");

	memset(&ea, 0, sizeof(ea));
	ea.grfAccessPermissions = EVENT_ALL_ACCESS;
	ea.grfAccessMode = SET_ACCESS;
	ea.grfInheritance = NO_INHERITANCE;
	ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
	ea.Trustee.ptstrName = (LPWSTR)sid;

	if (SetEntriesInAclW(1, &ea, nullptr, &acl) != ERROR_SUCCESS) {
		LocalFree(sid);
		throw DriverHostError("px4::DriverHost::Run: SetEntriesInAclW() failed.");
	}

	InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
	SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE);

	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = &sd;
	sa.bInheritHandle = FALSE;

	startup_event_ = CreateEventW(&sa, TRUE, FALSE, L"DriverHost_PX4_StartupEvent");
	if (!startup_event_)
		throw DriverHostError("px4::DriverHost::Run: CreateEventW() failed.");

	LocalFree(sid);

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
}

} // namespace px4
