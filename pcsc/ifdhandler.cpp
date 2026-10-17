// SPDX-License-Identifier: GPL-2.0-only
extern "C" {
#include <ifdhandler.h>
#include <debuglog.h>
}

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

#include "linux_card_device.hpp"
#include "smart_card.hpp"

namespace {

constexpr unsigned char MAX_READERS = 16;

struct Reader {
	std::mutex lock;
	std::shared_ptr<px4::LinuxCardDevice> device = std::make_shared<px4::LinuxCardDevice>();
	px4::SmartCard card{device};
	std::vector<std::uint8_t> atr;
	bool connected = true;
	bool powered = false;
	DWORD protocol = SCARD_PROTOCOL_UNDEFINED;

	void Invalidate()
	{
		card.Close();
		atr.clear();
		powered = false;
		protocol = SCARD_PROTOCOL_UNDEFINED;
	}
};

std::mutex readers_lock;
std::map<DWORD, std::shared_ptr<Reader>> readers;

RESPONSECODE MapError(int error)
{
	switch (error) {
	case 0: return IFD_SUCCESS;
	case -ENODEV: case -ENOENT: case -ENXIO: case -ESHUTDOWN: return IFD_NO_SUCH_DEVICE;
	case px4::SMART_CARD_NO_MEDIUM: case -ENOMEDIUM: return IFD_ICC_NOT_PRESENT;
	case -ETIMEDOUT: return IFD_RESPONSE_TIMEOUT;
	case -ENOBUFS: return IFD_ERROR_INSUFFICIENT_BUFFER;
	case -EOPNOTSUPP: return IFD_PROTOCOL_NOT_SUPPORTED;
	default: return IFD_COMMUNICATION_ERROR;
	}
}

template<class Function>
RESPONSECODE WithReader(DWORD lun, Function function)
{
	/* 一覧のロックを I/O の間も保持すると、別のリーダーの通信や抜去の処理まで待たせてしまう */
	std::shared_ptr<Reader> reader;
	try {
		{
			std::lock_guard<std::mutex> lock(readers_lock);
			auto found = readers.find(lun);
			if (found == readers.end())
				return IFD_NO_SUCH_DEVICE;
			reader = found->second;
		}
		std::lock_guard<std::mutex> lock(reader->lock);
		if (!reader->connected)
			return IFD_NO_SUCH_DEVICE;
		try {
			return function(*reader);
		} catch (...) {
			reader->Invalidate();
			return IFD_COMMUNICATION_ERROR;
		}
	} catch (...) {
		return IFD_COMMUNICATION_ERROR;
	}
}

int CheckPresence(Reader &reader, bool &present)
{
	int ret = reader.device->DetectCard(present);
	/* 通信の失敗と同じく、一時的な USB の失敗ではカードを閉じず、カードの抜去と USB の切断のときだけ閉じる */
	if (ret ? MapError(ret) == IFD_NO_SUCH_DEVICE : !present)
		reader.Invalidate();
	return ret;
}

} // namespace

extern "C" RESPONSECODE IFDHCreateChannelByName(DWORD Lun, LPSTR DeviceName)
{
	if (!DeviceName || (Lun & 0xffff) != 0)
		return IFD_NO_SUCH_DEVICE;
	try {
		auto reader = std::make_shared<Reader>();
		std::string path;
		int ret;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		do {
			ret = px4::ResolveCardDevice(DeviceName, path);
			if (!ret)
				ret = reader->device->Connect(path);
			if (ret != -ENODEV && ret != -ENOENT && ret != -EACCES)
				break;
			if (DeviceName[0] == '/')
				break;
			/*
			 * pcscd への USB の接続通知は、カーネルがカードデバイスを登録し終える前に届くことがある
			 * 登録の直後は root の 0600 で作られ、udev が pcscd グループの 0660 に直すまで、root 以外の pcscd は開けない
			 */
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		} while (std::chrono::steady_clock::now() < deadline);
		if (ret) {
			/* 再試行の途中の失敗は記録せず、権限の不足のような原因を最後の1回だけ pcscd のログに残す */
			if (!path.empty())
				Log4(PCSC_LOG_ERROR, "open(%s) failed: %s (errno=%d)",
					path.c_str(), std::strerror(-ret), -ret);
			return MapError(ret);
		}
		std::lock_guard<std::mutex> lock(readers_lock);
		if (readers.size() >= MAX_READERS || readers.count(Lun))
			return IFD_COMMUNICATION_ERROR;
		readers.emplace(Lun, std::move(reader));
		return IFD_SUCCESS;
	} catch (...) {
		return IFD_COMMUNICATION_ERROR;
	}
}

extern "C" RESPONSECODE IFDHCreateChannel(DWORD, DWORD)
{
	return IFD_NOT_SUPPORTED;
}

extern "C" RESPONSECODE IFDHCloseChannel(DWORD Lun)
{
	std::shared_ptr<Reader> reader;
	try {
		{
			std::lock_guard<std::mutex> lock(readers_lock);
			auto found = readers.find(Lun);
			if (found == readers.end())
				return IFD_NO_SUCH_DEVICE;
			reader = found->second;
			readers.erase(found);
		}
		std::lock_guard<std::mutex> lock(reader->lock);
		reader->connected = false;
		reader->Invalidate();
		reader->device->Disconnect();
		return IFD_SUCCESS;
	} catch (...) {
		return IFD_COMMUNICATION_ERROR;
	}
}

extern "C" RESPONSECODE IFDHGetCapabilities(DWORD Lun, DWORD Tag, PDWORD Length, PUCHAR Value)
{
	if (!Length)
		return IFD_COMMUNICATION_ERROR;
	const DWORD capacity = *Length;
	*Length = 0;
	return WithReader(Lun, [&](Reader &reader) -> RESPONSECODE {
		unsigned char value = 1;
		const void *data = &value;
		DWORD size = 1;
		switch (Tag) {
		case TAG_IFD_ATR:
			data = reader.atr.data();
			size = reader.atr.size();
			break;
		case TAG_IFD_THREAD_SAFE:
		case TAG_IFD_SLOT_THREAD_SAFE:
		case TAG_IFD_SLOTS_NUMBER:
			break;
		case TAG_IFD_SIMULTANEOUS_ACCESS:
			value = MAX_READERS;
			break;
		default:
			return IFD_ERROR_TAG;
		}
		*Length = size;
		if (capacity < size)
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		if (size && !Value) {
			*Length = 0;
			return IFD_COMMUNICATION_ERROR;
		}
		if (size)
			std::memcpy(Value, data, size);
		return IFD_SUCCESS;
	});
}

extern "C" RESPONSECODE IFDHSetCapabilities(DWORD, DWORD, DWORD, PUCHAR)
{
	return IFD_ERROR_TAG;
}

extern "C" RESPONSECODE IFDHICCPresence(DWORD Lun)
{
	return WithReader(Lun, [](Reader &reader) -> RESPONSECODE {
		bool present = false;
		int ret = CheckPresence(reader, present);
		return ret ? MapError(ret) : present ? IFD_ICC_PRESENT : IFD_ICC_NOT_PRESENT;
	});
}

extern "C" RESPONSECODE IFDHPowerICC(DWORD Lun, DWORD Action, PUCHAR Atr, PDWORD AtrLength)
{
	if (!AtrLength)
		return IFD_COMMUNICATION_ERROR;
	const DWORD capacity = *AtrLength;
	*AtrLength = 0;
	return WithReader(Lun, [&](Reader &reader) -> RESPONSECODE {
		if (Action == IFD_POWER_DOWN) {
			/* SmartCard::Close() は失敗を返さないため、電源の停止を先に実行して結果を pcscd へ返す */
			int ret = reader.device->PowerOff();
			reader.Invalidate();
			return MapError(ret);
		}
		if (Action != IFD_POWER_UP && Action != IFD_RESET)
			return IFD_NOT_SUPPORTED;
		if (!Atr || capacity < MAX_ATR_SIZE)
			return IFD_ERROR_INSUFFICIENT_BUFFER;
		bool present = false;
		int ret = CheckPresence(reader, present);
		if (ret || !present)
			return ret ? MapError(ret) : IFD_ICC_NOT_PRESENT;
		if (reader.powered)
			ret = reader.card.Reset(reader.atr);
		else {
			ret = reader.card.Open();
			if (!ret) {
				bool initialized = false;
				ret = reader.card.GetStatus(present, initialized, reader.atr);
				if (!ret && (!present || !initialized))
					ret = px4::SMART_CARD_NO_MEDIUM;
			}
		}
		if (ret) {
			reader.Invalidate();
			return MapError(ret);
		}
		reader.powered = true;
		reader.protocol = SCARD_PROTOCOL_UNDEFINED;
		std::memcpy(Atr, reader.atr.data(), reader.atr.size());
		*AtrLength = reader.atr.size();
		return IFD_SUCCESS;
	});
}

extern "C" RESPONSECODE IFDHSetProtocolParameters(DWORD Lun, DWORD Protocol,
	UCHAR Flags, UCHAR, UCHAR, UCHAR)
{
	return WithReader(Lun, [&](Reader &reader) -> RESPONSECODE {
		if (Protocol != SCARD_PROTOCOL_T1)
			return IFD_PROTOCOL_NOT_SUPPORTED;
		/* PPS の交渉には対応していないため、Flags で要求されたら成功を返さずに失敗させる */
		if (Flags)
			return IFD_ERROR_PTS_FAILURE;
		if (!reader.powered)
			return IFD_COMMUNICATION_ERROR;
		reader.protocol = Protocol;
		return IFD_SUCCESS;
	});
}

extern "C" RESPONSECODE IFDHTransmitToICC(DWORD Lun, SCARD_IO_HEADER SendPci,
	PUCHAR TxBuffer, DWORD TxLength, PUCHAR RxBuffer, PDWORD RxLength,
	PSCARD_IO_HEADER RecvPci)
{
	if (!RxLength)
		return IFD_COMMUNICATION_ERROR;
	const DWORD capacity = *RxLength;
	*RxLength = 0;
	return WithReader(Lun, [&](Reader &reader) -> RESPONSECODE {
		/* SendPci.Protocol はプロトコルの番号 (T=1 なら 1) で、ビットマスクの SCARD_PROTOCOL_T1 とは値が異なる */
		if (SendPci.Protocol != 1 || reader.protocol != SCARD_PROTOCOL_T1)
			return IFD_PROTOCOL_NOT_SUPPORTED;
		if (!reader.powered || !TxBuffer || !TxLength || !RxBuffer || !capacity)
			return IFD_COMMUNICATION_ERROR;
		std::size_t length = capacity;
		int ret = reader.card.Transmit(TxBuffer, TxLength, RxBuffer, length);
		if (ret) {
			/*
			 * 通信の失敗では SmartCard が自分の T=1 のセッションだけを捨て、次の APDU の前にカードをリセットして復帰する (Windows 版と同じ)
			 * pcscd は通信の失敗でカードをリセットしないため、ここでカードを閉じると、アプリケーションがリセットするまで通信できなくなる
			 */
			const RESPONSECODE code = MapError(ret);
			if (code == IFD_ICC_NOT_PRESENT || code == IFD_NO_SUCH_DEVICE)
				reader.Invalidate();
			return code;
		}
		*RxLength = length;
		if (RecvPci) {
			/* RecvPci はアプリケーションへそのまま渡り、ビットマスクとして読まれるため、番号の 1 を返すと T=0 と解釈される */
			RecvPci->Protocol = SCARD_PROTOCOL_T1;
			RecvPci->Length = sizeof(*RecvPci);
		}
		return IFD_SUCCESS;
	});
}

extern "C" RESPONSECODE IFDHControl(DWORD, DWORD, PUCHAR, DWORD, PUCHAR, DWORD,
	LPDWORD BytesReturned)
{
	if (BytesReturned)
		*BytesReturned = 0;
	return IFD_NOT_SUPPORTED;
}
