// security_attributes.cpp

#include "security_attributes.hpp"

namespace px4 {

SecurityAttributes::SecurityAttributes(DWORD permissions)
{
	sia_ = SECURITY_WORLD_SID_AUTHORITY;

	if (!AllocateAndInitializeSid(&sia_, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &sid_))
		throw SecurityAttributesError("px4::SecurityAttributes::SecurityAttributes: AllocateAndInitializeSid() failed.");

	memset(&ea_, 0, sizeof(ea_));
	ea_.grfAccessPermissions = permissions;
	ea_.grfAccessMode = SET_ACCESS;
	ea_.grfInheritance = NO_INHERITANCE;
	ea_.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	ea_.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
	ea_.Trustee.ptstrName = (LPWSTR)sid_;

	if (SetEntriesInAclW(1, &ea_, nullptr, &acl_) != ERROR_SUCCESS) {
		FreeSid(sid_);
		throw SecurityAttributesError("px4::SecurityAttributes::SecurityAttributes: SetEntriesInAclW() failed.");
	}

	InitializeSecurityDescriptor(&sd_, SECURITY_DESCRIPTOR_REVISION);
	SetSecurityDescriptorDacl(&sd_, TRUE, acl_, FALSE);

	sa_.nLength = sizeof(sa_);
	sa_.lpSecurityDescriptor = &sd_;
	sa_.bInheritHandle = FALSE;
}

SecurityAttributes::~SecurityAttributes()
{
	LocalFree(acl_);
	FreeSid(sid_);
}

SECURITY_ATTRIBUTES* SecurityAttributes::Get()
{
	return &sa_;
}

} // namespace px4
