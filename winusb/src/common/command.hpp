// command.hpp

#pragma once

#include <cstdint>
#include <cwchar>
#include <guiddef.h>

#include "type.hpp"

namespace px4 {

#pragma pack(push, 8)

namespace command {
	static const std::uint32_t VERSION = 0x00040002U;

	enum class CtrlCmdCode : std::uint32_t {
		UNDEFINED = 0,
		GET_VERSION = 1,
		OPEN = 8,
		CLOSE,
		GET_INFO,
		SET_CAPTURE,
		GET_PARAMS = 16,
		SET_PARAMS,
		CLEAR_PARAMS,
		TUNE,
		CHECK_LOCK,
		SET_LNB_VOLTAGE = 24,
		READ_STATS = 32,
	};

	enum class CtrlStatusCode : std::uint32_t {
		NONE = 0,	// request
		SUCCEEDED,	// response
		FAILED,		// response
	};

	struct CtrlCmdHeader {
		CtrlCmdCode cmd;
		CtrlStatusCode status;
	};

	struct CtrlVersionCmd : CtrlCmdHeader {
		std::uint32_t driver_version;
		std::uint32_t cmd_version;
	};

	struct ReceiverInfo {
		wchar_t device_name[96];
		GUID device_guid;		// not 'DeviceInterfaceGUID'
		wchar_t receiver_name[96];
		GUID receiver_guid;
		px4::SystemType systems;
		std::int32_t index;
		std::uint32_t data_id;
	};

	struct CtrlOpenCmd : CtrlCmdHeader {
		ReceiverInfo receiver_info;
	};

	struct CtrlCloseCmd : CtrlCmdHeader {
		// header only
	};

	struct CtrlReceiverInfoCmd : CtrlCmdHeader {
		ReceiverInfo receiver_info;
	};

	struct CtrlCaptureCmd : CtrlCmdHeader {
		bool capture;
	};

	enum class ParameterType : std::uint32_t {
		UNDEFINED = 0,
		BANDWIDTH = 1,
		STREAM_ID = 16,
	};

	struct Parameter {
		ParameterType type;
		std::uint32_t value;
	};

	struct ParameterSet {
		px4::SystemType system;
		std::uint32_t freq;
		std::uint32_t num;
		Parameter params[1];
	};

	struct CtrlParamsCmd : CtrlCmdHeader {
		ParameterSet param_set;
	};

	struct CtrlClearParamsCmd : CtrlCmdHeader {
		// header only
	};

	struct CtrlTuneCmd : CtrlCmdHeader {
		std::uint32_t timeout;
	};

	struct CtrlCheckLockCmd : CtrlCmdHeader {
		bool locked;
	};

	struct CtrlLnbVoltageCmd : CtrlCmdHeader {
		std::int32_t voltage;
	};

	enum class StatType : std::uint32_t {
		UNDEFINED = 0,
		SIGNAL_STRENGTH,
		CNR,
	};

	struct Stat {
		StatType type;
		std::int32_t value;
	};

	struct StatSet {
		std::uint32_t num;
		Stat data[1];
	};

	struct CtrlStatsCmd : CtrlCmdHeader {
		StatSet stat_set;
	};

	enum class DataCmdCode : std::uint32_t {
		UNDEFINED = 0,
		SET_DATA_ID = 1,
		PURGE = 8,
	};

	struct DataCmd {
		DataCmdCode cmd;
		union {
			std::uint32_t data_id;
		};
	};

} // namespace command

#pragma pack(pop)

} // namespace px4

