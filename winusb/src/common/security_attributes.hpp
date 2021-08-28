// security_attributes.hpp

#pragma once

#include <stdexcept>

#include <windows.h>
#include <aclapi.h>

namespace px4 {

class SecurityAttributes final {
public:
	explicit SecurityAttributes(DWORD permissions);
	~SecurityAttributes();

	SECURITY_ATTRIBUTES* Get();

private:
	SID_IDENTIFIER_AUTHORITY sia_;
	PSID sid_;
	EXPLICIT_ACCESS_W ea_;
	PACL acl_;
	SECURITY_DESCRIPTOR sd_;
	SECURITY_ATTRIBUTES sa_;
};

class SecurityAttributesError : public std::runtime_error {
public:
	explicit SecurityAttributesError(const std::string &what_arg) : runtime_error(what_arg.c_str()) {};
};

} // namespace px4
