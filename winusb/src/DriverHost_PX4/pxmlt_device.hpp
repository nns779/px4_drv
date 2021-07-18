// pxmlt_device.hpp

#pragma once

#include <cstdint>
#include <memory>
#include <atomic>
#include <string>
#include <mutex>
#include <condition_variable>

#include "device_definition_set.hpp"
#include "device_base.hpp"
#include "receiver_base.hpp"
#include "receiver_manager.hpp"
#include "ringbuffer.hpp"

#include "winusb_compat.h"

#include "i2c_comm.h"
#include "it930x.h"
#include "itedtv_bus.h"
#include "cxd2856er.h"
#include "cxd2858er.h"

namespace px4 {

#define PXMLT_DEVICE_TS_SYNC_COUNT	4
#define PXMLT_DEVICE_TS_SYNC_SIZE	(188 * PXMLT_DEVICE_TS_SYNC_COUNT)

enum class PxMltDeviceModel {
	PXMLT5U = 0,
	PXMLT5PE,
	PXMLT8PE3,
	PXMLT8PE5,
	ISDB6014_4TS
};

struct PxMltDeviceConfig final {
	PxMltDeviceConfig()
		: usb{ 816, 816, 5, false },
		device{ 2048, 2000, true }
	{}
	struct {
		unsigned int xfer_packets;
		unsigned int urb_max_packets;
		unsigned int max_urbs;
		bool no_raw_io;
	} usb;
	struct {
		unsigned int receiver_max_packets;
		int psb_purge_timeout;
		bool discard_null_packets;
	} device;
};

class PxMltDevice final : public px4::DeviceBase {
public:
	PxMltDevice() = delete;
	PxMltDevice(const std::wstring &path, const px4::DeviceDefinition &device_def, std::uintptr_t index, px4::ReceiverManager &receiver_manager);
	~PxMltDevice();

	// cannot copy
	PxMltDevice(const PxMltDevice &) = delete;
	PxMltDevice& operator=(const PxMltDevice &) = delete;

	// cannot move
	PxMltDevice(PxMltDevice &&) = delete;
	PxMltDevice& operator=(PxMltDevice &&) = delete;

	int Init() override;
	void Term() override;
	void SetAvailability(bool available) override;
	px4::ReceiverBase* GetReceiver(int id) const override;

private:
	struct StreamContext final {
		std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf[5];
		std::uint8_t remain_buf[PXMLT_DEVICE_TS_SYNC_SIZE];
		std::size_t remain_len;
	};

	class PxMltReceiver final : public px4::ReceiverBase {
	public:
		PxMltReceiver() = delete;
		PxMltReceiver(PxMltDevice &parent, std::uintptr_t index);
		~PxMltReceiver();

		// cannot copy
		PxMltReceiver(const PxMltReceiver &) = delete;
		PxMltReceiver& operator=(const PxMltReceiver &) = delete;

		// cannot move
		PxMltReceiver(PxMltReceiver &&) = delete;
		PxMltReceiver& operator=(PxMltReceiver &&) = delete;

		int Open() override;
		void Close() override;
		int CheckLock(bool &locked) override;
		int SetLnbVoltage(std::int32_t voltage) override;
		int SetCapture(bool capture) override;
		int ReadStat(px4::command::StatType type, std::int32_t &value) override;

	protected:
		int SetFrequency() override;
		int SetStreamId() override;

	private:
		static const struct PxMltReceiverCnTableIsdbS final {
			uint16_t val;
			uint32_t cnr;
		} isdbs_cn_table_[];

		PxMltDevice &parent_;
		std::uintptr_t index_;

		std::mutex lock_;
		std::atomic_bool open_;
		std::condition_variable close_cond_;
		bool lnb_power_;
		std::mutex *tuner_lock_;
		cxd2856er_demod cxd2856er_;
		cxd2858er_tuner cxd2858er_;
		px4::SystemType current_system_;
		bool streaming_;
	};

	void LoadConfig();

	const i2c_comm_master& GetI2cMaster(int bus) const;

	int SetBackendPower(bool state);
	int SetLnbVoltage(std::int32_t voltage);

	int StartCapture();
	int StopCapture();

	static void StreamProcess(std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf[], int num, std::uint8_t **buf, std::size_t &len);
	static int StreamHandler(void *context, void *buf, std::uint32_t len);

	static const struct PxMltDeviceParam final {
		std::uint8_t i2c_addr;
		std::uint8_t i2c_bus;
		std::uint8_t port_number;
	} params_[][5];

	PxMltDeviceModel model_;
	PxMltDeviceConfig config_;
	std::recursive_mutex lock_;
	std::atomic_bool available_;
	std::atomic_bool init_;
	unsigned int open_count_;
	unsigned int lnb_power_count_;
	unsigned int streaming_count_;
	std::mutex tuner_lock_[2];
	int receiver_num_;
	std::unique_ptr<PxMltReceiver> receivers_[5];
	it930x_bridge it930x_;
	StreamContext stream_ctx_;
};

} // namespace px4
