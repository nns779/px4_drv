// isdbt2071_device.hpp

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

namespace px4 {

#define ISDBT2071_DEVICE_TS_SYNC_COUNT	4U
#define ISDBT2071_DEVICE_TS_SYNC_SIZE		(188U * ISDBT2071_DEVICE_TS_SYNC_COUNT)

enum class Isdbt2071DeviceModel {
	ISDBT2071 = 0,
	OTHER,
};

struct Isdbt2071DeviceConfig final {
	Isdbt2071DeviceConfig()
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

class Isdbt2071Device final : public px4::DeviceBase {
public:
	Isdbt2071Device() = delete;
	Isdbt2071Device(const std::wstring &path, const px4::DeviceDefinition &device_def, std::uintptr_t index, px4::ReceiverManager &receiver_manager);
	~Isdbt2071Device();

	// cannot copy
	Isdbt2071Device(const Isdbt2071Device &) = delete;
	Isdbt2071Device& operator=(const Isdbt2071Device &) = delete;

	// cannot move
	Isdbt2071Device(Isdbt2071Device &&) = delete;
	Isdbt2071Device& operator=(Isdbt2071Device &&) = delete;

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
		std::uint8_t remain_buf[ISDBT2071_DEVICE_TS_SYNC_SIZE] = {};
		std::size_t remain_len = 0;
	};

	class Isdbt2071Receiver final : public px4::ReceiverBase {
	public:
		Isdbt2071Receiver() = delete;
		Isdbt2071Receiver(Isdbt2071Device &parent, std::uintptr_t index);
		~Isdbt2071Receiver();

		// cannot copy
		Isdbt2071Receiver(const Isdbt2071Receiver &) = delete;
		Isdbt2071Receiver& operator=(const Isdbt2071Receiver &) = delete;

		// cannot move
		Isdbt2071Receiver(Isdbt2071Receiver &&) = delete;
		Isdbt2071Receiver& operator=(Isdbt2071Receiver &&) = delete;

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

		Isdbt2071Device &parent_;
		std::uintptr_t index_;

		std::mutex lock_;
		std::atomic_bool init_;
		std::atomic_bool open_;
		std::condition_variable close_cond_;
		tc90522_demod tc90522_t_;
		r850_tuner r850_;
		px4::SystemType system_;
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

	Isdbt2071DeviceModel model_;
	Isdbt2071DeviceConfig config_;
	std::recursive_mutex lock_;
	std::atomic_bool available_;
	std::atomic_bool init_;
	unsigned int streaming_count_;
	bool card_open_;
	bool receiver_open_;
	std::unique_ptr<Isdbt2071Receiver> receiver_;
	it930x_bridge it930x_;
	StreamContext stream_ctx_;
};

} // namespace px4
