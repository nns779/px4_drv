// isdb2056_device.hpp

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
#include "tc90522.h"
#include "r850.h"
#include "rt710.h"

namespace px4 {

#define ISDB2056_DEVICE_TS_SYNC_COUNT	4U
#define ISDB2056_DEVICE_TS_SYNC_SIZE		(188U * ISDB2056_DEVICE_TS_SYNC_COUNT)

enum class Isdb2056DeviceModel {
	ISDB2056 = 0,
	ISDB2056N,
	PXM1UR,
	PXS1UR,
	OTHER,
};

struct Isdb2056DeviceConfig final {
	Isdb2056DeviceConfig()
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

class Isdb2056Device final : public px4::DeviceBase {
public:
	Isdb2056Device() = delete;
	Isdb2056Device(const std::wstring &path, const px4::DeviceDefinition &device_def, std::uintptr_t index, px4::ReceiverManager &receiver_manager);
	~Isdb2056Device();

	// cannot copy
	Isdb2056Device(const Isdb2056Device &) = delete;
	Isdb2056Device& operator=(const Isdb2056Device &) = delete;

	// cannot move
	Isdb2056Device(Isdb2056Device &&) = delete;
	Isdb2056Device& operator=(Isdb2056Device &&) = delete;

	int Init() override;
	void Term() override;
	void SetAvailability(bool available) override;
	px4::ReceiverBase * GetReceiver(int id) const override;
	bool HasCardReader() const noexcept override { return true; }
	int OpenCard() override;
	void CloseCard() override;
	int DetectCard(bool &detected) override;
	int ResetCard() override;
	int SetCardBaudrate(std::uint32_t baudrate) override;
	int IsCardDataReady(bool &ready) override;
	int ReadCardData(std::uint8_t *buf, std::uint8_t &len) override;
	int WriteCardData(const std::uint8_t *buf, std::uint8_t len) override;

private:
	struct StreamContext final {
		std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf;
		/* 最初のコールバック前も未処理データなしの状態として初期化 */
		std::uint8_t remain_buf[ISDB2056_DEVICE_TS_SYNC_SIZE] = {};
		std::size_t remain_len = 0;
	};

	class Isdb2056Receiver final : public px4::ReceiverBase {
	public:
		Isdb2056Receiver() = delete;
		Isdb2056Receiver(Isdb2056Device &parent, std::uintptr_t index);
		~Isdb2056Receiver();

		// cannot copy
		Isdb2056Receiver(const Isdb2056Receiver &) = delete;
		Isdb2056Receiver& operator=(const Isdb2056Receiver &) = delete;

		// cannot move
		Isdb2056Receiver(Isdb2056Receiver &&) = delete;
		Isdb2056Receiver& operator=(Isdb2056Receiver &&) = delete;

		int Init(bool sleep);
		void Term();

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
		static struct tc90522_regbuf tc_init_t_[];
		static struct tc90522_regbuf tc_init_s_[];

		Isdb2056Device &parent_;
		std::uintptr_t index_;

		std::mutex lock_;
		std::atomic_bool init_;
		std::atomic_bool open_;
		std::condition_variable close_cond_;
		bool lnb_power_;
		tc90522_demod tc90522_t_;
		tc90522_demod tc90522_s0_;
		tc90522_demod tc90522_s_;
		r850_tuner r850_;
		rt710_tuner rt710_;
		px4::SystemType current_system_;
		std::atomic_bool streaming_;
	};

	void LoadConfig();

	const i2c_comm_master& GetI2cMaster(int bus) const;

	int SetBackendPower(bool state);
	int SetLnbVoltage(std::int32_t voltage);

	int PrepareCapture();
	int StartCapture();
	int StopCapture();

	static void StreamProcess(std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf, std::uint8_t **buf, std::size_t &len);
	static int StreamHandler(void *context, void *buf, std::uint32_t len);

	Isdb2056DeviceModel model_;
	Isdb2056DeviceConfig config_;
	std::recursive_mutex lock_;
	std::atomic_bool available_;
	std::atomic_bool init_;
	unsigned int streaming_count_;
	bool card_open_;
	bool receiver_open_;
	std::unique_ptr<Isdb2056Receiver> receiver_;
	it930x_bridge it930x_;
	StreamContext stream_ctx_;
};

} // namespace px4
