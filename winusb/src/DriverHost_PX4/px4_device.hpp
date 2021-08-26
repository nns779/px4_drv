// px4_device.hpp

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

#define PX4_DEVICE_TS_SYNC_COUNT	4U
#define PX4_DEVICE_TS_SYNC_SIZE		(188U * PX4_DEVICE_TS_SYNC_COUNT)

enum class Px4MultiDeviceMode {
	ALL = 0,
	S_ONLY,
	S0_ONLY,
	S1_ONLY
};

struct Px4DeviceConfig final {
	Px4DeviceConfig()
		: usb{ 816, 816, 5, false },
		device{ 2048, 2000, false, Px4MultiDeviceMode::ALL, true }
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
		bool disable_multi_device_power_control;
		Px4MultiDeviceMode multi_device_power_control_mode;
		bool discard_null_packets;
	} device;
};

class Px4Device final : public px4::DeviceBase {
public:
	Px4Device() = delete;
	Px4Device(const std::wstring &path, const px4::DeviceDefinition &device_def, std::uintptr_t index, px4::ReceiverManager &receiver_manager);
	~Px4Device();

	// cannot copy
	Px4Device(const Px4Device &) = delete;
	Px4Device& operator=(const Px4Device &) = delete;

	// cannot move
	Px4Device(Px4Device &&) = delete;
	Px4Device& operator=(Px4Device &&) = delete;

	int Init() override;
	void Term() override;
	void SetAvailability(bool available) override;
	px4::ReceiverBase * GetReceiver(int id) const override;

private:
	struct SerialNumber final {
		SerialNumber() : serial_number(0), dev_id(0) {}
		std::uint64_t serial_number;
		std::uint8_t dev_id;
	};
	struct StreamContext final {
		std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf[4];
		std::uint8_t remain_buf[PX4_DEVICE_TS_SYNC_SIZE];
		std::size_t remain_len;
	};

	class MultiDevice final {
	public:
		~MultiDevice();

		// cannot copy
		MultiDevice(const MultiDevice &) = delete;
		MultiDevice& operator=(const MultiDevice &) = delete;

		// cannot move
		MultiDevice(MultiDevice &&) = delete;
		MultiDevice& operator=(MultiDevice &&) = delete;

		static bool Search(std::uint64_t serial_number, std::shared_ptr<MultiDevice> &mldev);
		static int Alloc(Px4Device &dev, Px4MultiDeviceMode mode, std::shared_ptr<MultiDevice> &mldev);

		int Add(Px4Device &dev);
		int Remove(Px4Device &dev);
		int SetPower(Px4Device &dev, std::uintptr_t index, bool state, bool *first);

	private:
		MultiDevice(Px4MultiDeviceMode mode, std::uint64_t serial_number);

		std::uint8_t GetDeviceCount() const noexcept;
		bool GetReceiverStatus(std::uint8_t dev_id) const noexcept;
		bool IsPowerIntelockingRequried(std::uint8_t dev_id) const noexcept;

		static std::mutex mldev_list_lock_;
		static std::unordered_map<std::uint64_t, std::shared_ptr<MultiDevice>> mldev_list_;

		std::mutex lock_;
		std::uint64_t serial_number_;
		Px4MultiDeviceMode mode_;
		Px4Device *dev_[2];
		bool power_state_[2];
		bool receiver_state_[2][4];
	};

	class Px4Receiver final : public px4::ReceiverBase {
	public:
		Px4Receiver() = delete;
		Px4Receiver(Px4Device &parent, std::uintptr_t index);
		~Px4Receiver();

		// cannot copy
		Px4Receiver(const Px4Receiver &) = delete;
		Px4Receiver& operator=(const Px4Receiver &) = delete;

		// cannot move
		Px4Receiver(Px4Receiver &&) = delete;
		Px4Receiver& operator=(Px4Receiver &&) = delete;

		int Init(bool sleep);
		void Term();

		int Open() override;
		void Close() override;
		int CheckLock(bool &locked) override;
		int SetLnbVoltage(std::int32_t voltage) override;
		int SetCapture(bool capture) override;
		int ReadStat(px4::command::StatType type, std::int32_t &value) override;

		int InitPrimary();

	protected:
		int SetFrequency() override;
		int SetStreamId() override;

	private:
		static struct tc90522_regbuf tc_init_t0_[];
		static struct tc90522_regbuf tc_init_s0_[];
		static struct tc90522_regbuf tc_init_t_[];
		static struct tc90522_regbuf tc_init_s_[];

		Px4Device &parent_;
		std::uintptr_t index_;

		std::mutex lock_;
		px4::SystemType system_;
		std::atomic_bool init_;
		std::atomic_bool open_;
		std::condition_variable close_cond_;
		bool lnb_power_;
		tc90522_demod tc90522_;
		union {
			r850_tuner r850_;
			rt710_tuner rt710_;
		};
		std::atomic_bool streaming_;
	};

	void LoadConfig();
	void ParseSerialNumber() noexcept;

	const i2c_comm_master& GetI2cMaster(int bus) const;

	int SetBackendPower(bool state);
	int SetLnbVoltage(std::int32_t voltage);

	int PrepareCapture();
	int StartCapture();
	int StopCapture();

	static void StreamProcess(std::shared_ptr<px4::ReceiverBase::StreamBuffer> stream_buf[], std::uint8_t **buf, std::size_t &len);
	static int StreamHandler(void *context, void *buf, std::uint32_t len);

	Px4DeviceConfig config_;
	std::recursive_mutex lock_;
	std::atomic_bool available_;
	SerialNumber serial_;
	std::shared_ptr<MultiDevice> mldev_;
	std::atomic_bool init_;
	unsigned int open_count_;
	unsigned int lnb_power_count_;
	unsigned int streaming_count_;
	std::unique_ptr<Px4Receiver> receivers_[4];
	it930x_bridge it930x_;
	StreamContext stream_ctx_;
};

} // namespace px4
