// SPDX-License-Identifier: GPL-2.0-only
extern "C" {
#include <ifdhandler.h>
#include <debuglog.h>
}

#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "linux_card_device.hpp"

/*
 * IFD ハンドラーの関数から LinuxCardDevice までは実物を通し、open・close・ioctl のシステムコールだけを模擬する
 * T=1 の再送などの状態遷移は、Windows 版と共有する smart_card_state_test で試験する
 */
namespace {
struct Device {
	bool opened = false;
	bool available = true;
	bool present = true;
	bool powered = false;
	bool old_module = false;
	int error = 0;
	int detect_error = 0;
	unsigned int resets = 0;
	unsigned int writes = 0;
	unsigned int sequence = 0;
	std::deque<unsigned char> rx;
};
std::array<Device, 2> devices;
constexpr int FIRST_FD = 50000;
unsigned char atr[] = {0x3b, 0x80, 0x01, 0x81};
std::string last_log;

int Failure(int error)
{
	errno = error;
	return -1;
}

Device *FromFd(int fd)
{
	return fd >= FIRST_FD && fd < FIRST_FD + int(devices.size()) ?
		&devices[fd - FIRST_FD] : nullptr;
}

void Queue(Device &device, unsigned char pcb, const unsigned char *data, unsigned int length)
{
	unsigned char lrc = pcb ^ length;
	device.rx.insert(device.rx.end(), {0, pcb, static_cast<unsigned char>(length)});
	for (unsigned int i = 0; i < length; i++) {
		device.rx.push_back(data[i]);
		lrc ^= data[i];
	}
	device.rx.push_back(lrc);
}
} // namespace

/* pcscd が提供するログ関数を模擬し、errno が変わる場合にも open() のエラーを返すことを確かめる */
extern "C" void log_msg(const int priority, const char *format, ...)
{
	assert(priority == PCSC_LOG_ERROR);
	char message[1024];
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	last_log = message;
	errno = EIO;
}

extern "C" int __real_open(const char *, int, ...);
extern "C" int __real_close(int);
extern "C" int __real_ioctl(int, unsigned long, ...);

extern "C" int __wrap_open(const char *path, int flags, ...)
{
	for (unsigned int i = 0; i < devices.size(); i++) {
		const std::string expected = "/dev/px4card-test" + std::to_string(i);
		if (path == expected) {
			if (devices[i].opened)
				return Failure(EBUSY);
			assert(flags & O_CLOEXEC);
			devices[i].opened = true;
			return FIRST_FD + i;
		}
	}
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list args;
		va_start(args, flags);
		mode = va_arg(args, int);
		va_end(args);
	}
	return __real_open(path, flags, mode);
}

extern "C" int __wrap_close(int fd)
{
	if (auto *device = FromFd(fd)) {
		device->opened = false;
		device->powered = false;
		return 0;
	}
	return __real_close(fd);
}

extern "C" int __wrap_ioctl(int fd, unsigned long command, ...)
{
	va_list args;
	va_start(args, command);
	void *argument = va_arg(args, void *);
	va_end(args);
	auto *device = FromFd(fd);
	if (!device)
		return __real_ioctl(fd, command, argument);
	if (!device->opened || !device->available)
		return Failure(ENODEV);
	/* カードのコマンドを知らないカーネルモジュールは ENOTTY を返す */
	if (device->old_module)
		return Failure(ENOTTY);
	auto &value = *static_cast<__u32 *>(argument);
	auto &frame = *static_cast<px4_card_frame *>(argument);
	switch (command) {
	case PX4_CARD_POWER_ON:
		device->powered = true;
		return 0;
	case PX4_CARD_POWER_OFF:
		device->powered = false;
		return 0;
	case PX4_CARD_DETECT:
		if (device->detect_error) {
			const int error = device->detect_error;
			device->detect_error = 0;
			return Failure(error);
		}
		value = device->present;
		return 0;
	}
	if (!device->powered)
		return Failure(EHOSTDOWN);
	switch (command) {
	case PX4_CARD_RESET:
		device->resets++;
		device->sequence = 0;
		device->rx.assign(std::begin(atr), std::end(atr));
		return 0;
	case PX4_CARD_SET_BAUDRATE:
		assert(value == 9600);
		return 0;
	case PX4_CARD_RX_READY:
		value = !device->rx.empty();
		return 0;
	case PX4_CARD_READ: {
		assert(frame.length);
		const auto length = std::min<std::size_t>(frame.length, device->rx.size());
		for (std::size_t i = 0; i < length; i++) {
			frame.data[i] = device->rx.front();
			device->rx.pop_front();
		}
		frame.length = length;
		return 0;
	}
	case PX4_CARD_WRITE:
		assert(frame.length);
		device->writes++;
		if (device->error) {
			const int error = device->error;
			device->error = 0;
			return Failure(error);
		}
		if ((frame.data[1] & 0xc0) == 0xc0)
			Queue(*device, frame.data[1] | 0x20, frame.data + 3, frame.data[2]);
		else {
			const unsigned char reply[] = {0x90, 0x00};
			Queue(*device, device->sequence ? 0x40 : 0, reply, sizeof(reply));
			device->sequence ^= 1;
		}
		return 0;
	default:
		return Failure(ENOTTY);
	}
}

#ifndef PX4_IFD_FAKE_MODULE
static void TestResolver()
{
	char pattern[] = "/tmp/px4-card-test-XXXXXX";
	char *directory = mkdtemp(pattern);
	assert(directory);
	const std::filesystem::path root(directory);
	std::filesystem::create_directories(root / "class/000042546005001");
	std::filesystem::create_directories(root / "usb/3-2/3-2:1.0");
	std::filesystem::create_directory_symlink(root / "usb/3-2/3-2:1.0",
		root / "class/000042546005001/device");
	std::ofstream(root / "usb/3-2/idVendor") << "0511\n";
	std::ofstream(root / "usb/3-2/idProduct") << "924e\n";
	std::ofstream(root / "usb/3-2/busnum") << "3\n";
	std::ofstream(root / "usb/3-2/devnum") << "8\n";
	std::ofstream(root / "class/000042546005001/uevent") << "MAJOR=10\nMINOR=123\nDEVNAME=px4card/000042546005001\n";
	std::string path;
	assert(px4::ResolveCardDevice("usb:0511/924e:libudev:0:/dev/bus/usb/003/008",
		path, (root / "class").string()) == 0 && path == "/dev/px4card/000042546005001");
	assert(px4::ResolveCardDevice("usb:0511/924e:libudev:0:/dev/bus/usb/003/009",
		path, (root / "class").string()) == -ENODEV);
	assert(px4::ResolveCardDevice("usb:0511/924e:libudev:1:/dev/bus/usb/003/008",
		path, (root / "class").string()) == -EINVAL);
	assert(px4::ResolveCardDevice("usb:0511/924e:libudev:0:/dev/bus/usb/003/008junk",
		path, (root / "class").string()) == -EINVAL);
	for (const char *devname : {"px4card/usb-3-2", "px4card/000042546005001-1"}) {
		std::ofstream(root / "class/000042546005001/uevent") << "DEVNAME=" << devname << "\n";
		assert(px4::ResolveCardDevice("usb:0511/924e:libudev:0:/dev/bus/usb/003/008",
			path, (root / "class").string(), (root / "dev").string()) == 0 &&
			path == (root / "dev" / devname).string());
	}
	std::ofstream(root / "class/000042546005001/uevent") << "MAJOR=10\nMINOR=123\n";
	assert(px4::ResolveCardDevice("usb:0511/924e:libudev:0:/dev/bus/usb/003/008",
		path, (root / "class").string()) == -ENODEV);
	for (const char *devname : {"", "/tmp/card", "px4card/../../tmp/card"}) {
		std::ofstream(root / "class/000042546005001/uevent") << "DEVNAME=" << devname << "\n";
		assert(px4::ResolveCardDevice("usb:0511/924e:libudev:0:/dev/bus/usb/003/008",
			path, (root / "class").string()) == -EINVAL);
	}
	std::filesystem::remove(root / "class/000042546005001/uevent");
	assert(px4::ResolveCardDevice("usb:0511/924e:libudev:0:/dev/bus/usb/003/008",
		path, (root / "class").string()) == -ENODEV);
	/* PX-Q3 系はシリアル番号の末尾の桁が 1 の側だけにカードスロットがあり、反対側は登録を待たずに失敗させる */
	std::filesystem::create_directories(root / "usb/3-3");
	std::ofstream(root / "usb/3-3/busnum") << "3\n";
	std::ofstream(root / "usb/3-3/devnum") << "9\n";
	const std::pair<const char *, int> q3_cases[] = {{"000001711009060", -ENXIO}, {"000001711009061", -ENODEV}};
	for (const auto &[serial, expected] : q3_cases) {
		std::ofstream(root / "usb/3-3/serial") << serial << "\n";
		assert(px4::ResolveCardDevice("usb:0511/084a:libudev:0:/dev/bus/usb/003/009",
			path, (root / "class").string(), "/dev", (root / "usb").string()) == expected);
	}
	std::filesystem::remove_all(root);
}

int main(int argc, char *argv[])
{
	/* ioctl の番号には構造体の大きさが入るため、配置が変わると既存のカーネルモジュールと通じなくなる */
	static_assert(sizeof(px4_card_frame) == 256, "UAPI layout changed");
	static_assert(offsetof(px4_card_frame, data) == 1, "UAPI layout changed");
	TestResolver();
	/* pcscd のログ関数を提供した状態で、導入した本番ライブラリのシンボルをすべて解決する */
	if (argc == 2) {
		void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
		if (!library)
			std::fprintf(stderr, "%s\n", dlerror());
		assert(library);
		dlclose(library);
	}
	char path0[] = "/dev/px4card-test0", path1[] = "/dev/px4card-test1";
	assert(IFDHCreateChannelByName(1, path0) == IFD_NO_SUCH_DEVICE);
	assert(IFDHCreateChannelByName(0, path0) == IFD_SUCCESS);
	assert(last_log.empty());
	assert(devices[0].opened && !devices[0].powered && devices[0].resets == 0);
	assert(IFDHCreateChannelByName(0x10000, path0) == IFD_COMMUNICATION_ERROR);
	assert(last_log.find(path0) != std::string::npos &&
		last_log.find("errno=" + std::to_string(EBUSY)) != std::string::npos);
	last_log.clear();
	px4::LinuxCardDevice busy;
	assert(busy.Connect(path0) == -EBUSY && last_log.empty());
	assert(IFDHCreateChannelByName(0x10000, path1) == IFD_SUCCESS);
	assert(last_log.empty());
	assert(IFDHICCPresence(0) == IFD_ICC_PRESENT);
	assert(devices[0].resets == 0); // 状態照会はカードをリセットしない
	unsigned char buffer[64]{};
	DWORD length = 0;
	assert(IFDHGetCapabilities(0, TAG_IFD_SLOTS_NUMBER, &length, buffer) == IFD_ERROR_INSUFFICIENT_BUFFER);
	assert(length == 1);
	length = sizeof(buffer);
	assert(IFDHGetCapabilities(0, TAG_IFD_SLOTS_NUMBER, &length, buffer) == IFD_SUCCESS && buffer[0] == 1);
	length = sizeof(buffer);
	assert(IFDHPowerICC(0, IFD_POWER_UP, buffer, &length) == IFD_SUCCESS);
	assert(length == sizeof(atr) && !std::memcmp(buffer, atr, length));
	assert(devices[0].powered && devices[0].resets == 1 && !devices[1].powered);
	assert(IFDHSetProtocolParameters(0, SCARD_PROTOCOL_T0, 0, 0, 0, 0) == IFD_PROTOCOL_NOT_SUPPORTED);
	assert(IFDHSetProtocolParameters(0, SCARD_PROTOCOL_T1, 1, 0x11, 0, 0) == IFD_ERROR_PTS_FAILURE);
	assert(IFDHSetProtocolParameters(0, SCARD_PROTOCOL_T1, 0, 0, 0, 0) == IFD_SUCCESS);
	SCARD_IO_HEADER pci{1, sizeof(SCARD_IO_HEADER)}, received{};
	unsigned char apdu[] = {0, 0x84, 0, 0, 0};
	for (int i = 0; i < 2; i++) {
		length = sizeof(buffer);
		assert(IFDHTransmitToICC(0, pci, apdu, sizeof(apdu), buffer, &length, &received) == IFD_SUCCESS);
		assert(length == 2 && buffer[0] == 0x90 && received.Protocol == SCARD_PROTOCOL_T1);
	}
	devices[0].error = ETIMEDOUT;
	length = sizeof(buffer);
	assert(IFDHTransmitToICC(0, pci, apdu, sizeof(apdu), buffer, &length, &received) == IFD_RESPONSE_TIMEOUT);
	assert(length == 0 && devices[0].powered); // 通信の失敗ではカードを閉じない
	length = sizeof(buffer);
	assert(IFDHTransmitToICC(0, pci, apdu, sizeof(apdu), buffer, &length, &received) == IFD_SUCCESS);
	assert(length == 2 && devices[0].resets == 2); // 次の APDU の前にカードをリセットして復帰する
	devices[0].detect_error = ETIMEDOUT;
	assert(IFDHICCPresence(0) == IFD_RESPONSE_TIMEOUT && devices[0].powered); // 有無の問い合わせの一時的な失敗でもカードを閉じない
	length = sizeof(buffer);
	assert(IFDHTransmitToICC(0, pci, apdu, sizeof(apdu), buffer, &length, &received) == IFD_SUCCESS);
	assert(devices[0].resets == 2);
	length = sizeof(buffer);
	assert(IFDHGetCapabilities(0, TAG_IFD_ATR, &length, buffer) == IFD_SUCCESS && length == sizeof(atr));
	length = sizeof(buffer);
	assert(IFDHPowerICC(0, IFD_RESET, buffer, &length) == IFD_SUCCESS);
	assert(devices[0].resets == 3);
	devices[0].present = false;
	length = sizeof(buffer);
	assert(IFDHSetProtocolParameters(0, SCARD_PROTOCOL_T1, 0, 0, 0, 0) == IFD_SUCCESS);
	assert(IFDHTransmitToICC(0, pci, apdu, sizeof(apdu), buffer, &length, &received) == IFD_ICC_NOT_PRESENT);
	assert(length == 0 && !devices[0].powered); // カードの抜去ではカードを閉じる
	devices[0].present = true;
	length = sizeof(buffer);
	assert(IFDHPowerICC(0, IFD_POWER_DOWN, buffer, &length) == IFD_SUCCESS && length == 0);
	const auto resets = devices[0].resets;
	assert(IFDHICCPresence(0) == IFD_ICC_PRESENT && devices[0].resets == resets && !devices[0].powered);
	devices[0].present = false;
	assert(IFDHICCPresence(0) == IFD_ICC_NOT_PRESENT);
	length = sizeof(buffer);
	assert(IFDHPowerICC(0, IFD_POWER_UP, buffer, &length) == IFD_ICC_NOT_PRESENT && length == 0);
	devices[0].present = true;
	length = sizeof(buffer);
	assert(IFDHPowerICC(0, IFD_POWER_UP, buffer, &length) == IFD_SUCCESS);
	devices[0].available = false;
	assert(IFDHICCPresence(0) == IFD_NO_SUCH_DEVICE);
	assert(IFDHICCPresence(0x10000) == IFD_ICC_PRESENT);
	assert(IFDHCloseChannel(0) == IFD_SUCCESS && !devices[0].opened);
	assert(IFDHCloseChannel(0) == IFD_NO_SUCH_DEVICE);
	assert(IFDHCloseChannel(0x10000) == IFD_SUCCESS && !devices[1].opened);
	devices[0].available = true;
	devices[0].old_module = true;
	assert(IFDHCreateChannelByName(0, path0) == IFD_SUCCESS);
	length = sizeof(buffer);
	assert(IFDHPowerICC(0, IFD_POWER_UP, buffer, &length) != IFD_SUCCESS && length == 0);
	assert(last_log.find("not supported by the px4_drv kernel module") != std::string::npos);
	assert(IFDHCloseChannel(0) == IFD_SUCCESS && !devices[0].opened);
	std::puts("ifdhandler_test: passed");
}
#endif
