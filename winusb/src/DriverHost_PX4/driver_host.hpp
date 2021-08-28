// driver_host.hpp

#pragma once

#include <memory>
#include <string>
#include <stdexcept>

#include <windows.h>

#include "config.hpp"
#include "device_definition_set.hpp"
#include "device_manager.hpp"
#include "receiver_manager.hpp"
#include "ctrl_server.hpp"
#include "stream_server.hpp"
#include "util.hpp"

namespace px4 {

class DriverHost final {
public:
	DriverHost();
	~DriverHost();

	// cannot copy
	DriverHost(const DriverHost &) = delete;
	DriverHost& operator=(const DriverHost &) = delete;

	// cannot move
	DriverHost(DriverHost &&) = delete;
	DriverHost& operator=(DriverHost &&) = delete;

	void Run();

private:
	px4::ConfigSet configs_;
	px4::DeviceDefinitionSet dev_defs_;
	px4::ReceiverManager receiver_manager_;

	HANDLE mutex_;
	HANDLE startup_event_;
	std::unique_ptr<px4::DeviceManager> device_manager_;
	std::unique_ptr<px4::CtrlServer> ctrl_server_;
	std::unique_ptr<px4::StreamServer> stream_server_;
};

class DriverHostError : public std::runtime_error {
public:
	explicit DriverHostError(const std::string &what_arg) : runtime_error(what_arg.c_str()) {};
};

} // namespace px4
