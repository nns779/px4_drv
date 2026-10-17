#include "smart_card.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace px4 {

namespace {

constexpr std::uint8_t T1_NAD = 0x00;
constexpr std::uint8_t T1_I_BLOCK = 0x00;
constexpr std::uint8_t T1_I_SEQUENCE = 0x40;
constexpr std::uint8_t T1_I_CHAIN = 0x20;
constexpr std::uint8_t T1_R_BLOCK = 0x80;
constexpr std::uint8_t T1_R_SEQUENCE = 0x10;
constexpr std::uint8_t T1_R_EDC_ERROR = 0x01;
constexpr std::uint8_t T1_S_BLOCK = 0xc0;
constexpr std::uint8_t T1_S_RESPONSE = 0x20;
constexpr std::uint8_t T1_S_RESYNCH = 0x00;
constexpr std::uint8_t T1_S_IFS = 0x01;
constexpr std::uint8_t T1_S_WTX = 0x03;
/* IT930x のカード UART API はフレーム長を1バイトで受け取る */
constexpr std::size_t CARD_UART_FRAME_MAX_LENGTH = 255;
constexpr unsigned int CARD_POLL_INTERVAL_MS = 5;
constexpr unsigned int ATR_TIMEOUT_MS = 1000;
/* 通常応答は約70msで返るため、取りこぼしを500msで再送処理へ移す */
constexpr unsigned int BLOCK_TIMEOUT_MS = 500;
constexpr unsigned int MAX_RETRIES = 3;
/*
 * 読み捨てる残余は直前の要求への遅延応答に限られ、ブロックの再送は3回で
 * 打ち切るため、すべて連結しても最大3フレームに収まり4回の読み出しで足りる
 */
constexpr unsigned int MAX_DISCARD_CHUNKS = 4;
/* USB と UART が競合した実機でも応答を待ちつつ、連続する WTX は3秒で打ち切る */
constexpr unsigned int OPERATION_TIMEOUT_MS = 3000;

} // namespace

SmartCard::SmartCard(std::shared_ptr<CardDevice> device) noexcept
	: device_(std::move(device)),
	open_(false),
	present_(false),
	initialized_(false),
	use_crc_(false),
	card_ifsc_(32),
	block_timeout_ms_(BLOCK_TIMEOUT_MS),
	send_sequence_(0),
	receive_sequence_(0)
{
}

SmartCard::~SmartCard()
{
	Close();
}

int SmartCard::Open()
{
	if (open_)
		return -EALREADY;

	int ret = device_->OpenCard();
	if (ret)
		return ret;

	open_ = true;
	bool detected = false;
	ret = device_->DetectCard(detected);
	if (ret) {
		Close();
		return ret;
	}
	/* カードなしでも UART を開いたまま待機し、後からの挿入を検出する */
	if (detected) {
		ret = Reset(atr_);
		if (ret) {
			Close();
			return ret;
		}
	}

	return 0;
}

void SmartCard::Close() noexcept
{
	if (!open_)
		return;

	device_->CloseCard();
	open_ = false;
	InvalidateSession();
}

int SmartCard::GetStatus(bool &present, bool &initialized,
			 std::vector<std::uint8_t> &atr)
{
	if (!open_)
		return -ENODEV;

	int ret = device_->DetectCard(present);
	if (ret) {
		InvalidateSession();
		return ret;
	}

	/* 抜去後の ATR と T=1 シーケンスは再挿入されたカードへ引き継がない */
	if (!present)
		InvalidateSession();
	else if (!initialized_) {
		/* 状態監視だけの利用者にも正しい ATR を返せるよう挿入時に初期化する */
		int reset_ret = Reset(atr);
		if (reset_ret)
			return reset_ret;
	}
	initialized = initialized_;
	atr = initialized_ ? atr_ : std::vector<std::uint8_t>();
	return 0;
}

int SmartCard::Reset(std::vector<std::uint8_t> &atr)
{
	if (!open_)
		return -ENODEV;

	/* 途中で失敗しても以前のカードの通信状態を再利用させない */
	InvalidateSession();
	bool detected = false;
	int ret = device_->DetectCard(detected);
	if (ret)
		return ret;
	if (!detected)
		return SMART_CARD_NO_MEDIUM;

	AtrParameters parameters;
	for (unsigned int attempt = 0; attempt < 2; attempt++) {
		/* 前回の不正 ATR を途中まで解析していても T=1 パラメータを次の試行へ残さない */
		parameters = AtrParameters{};
		ret = device_->ResetCard();
		if (ret)
			return ret;

		ret = ReadAtr(atr, parameters);
		if (!ret)
			break;
		/* 一部機種は連続リセット時に UART の残留バイトを ATR より先に返すため、形式不正だけ再初期化する */
		if (ret != -EBADMSG || attempt != 0)
			return ret;
	}

	/* 特定モードのカードは PPS を行わず、TA1 が指定した速度へ直接切り替える */
	ret = device_->SetCardBaudrate(parameters.baudrate);
	if (ret)
		return ret;

	atr_ = atr;
	present_ = true;
	card_ifsc_ = parameters.ifsc;
	use_crc_ = parameters.use_crc;
	block_timeout_ms_ = parameters.block_timeout_ms;
	const bool is_acas = parameters.baudrate == 38400;
	/* ACAS は IFS (254) から開始し、B-CAS は従来の RESYNCH を先に送る */
	const std::uint8_t ifsd = is_acas ? 254 : (use_crc_ ? 250 : 251);
	ret = InitializeT1(!is_acas, ifsd);
	if (ret) {
		InvalidateSession();
		return ret;
	}
	initialized_ = true;
	return 0;
}

int SmartCard::ReadAtr(std::vector<std::uint8_t> &atr, AtrParameters &parameters)
{
	atr.clear();
	auto deadline = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(ATR_TIMEOUT_MS);
	std::size_t expected_length = 0;

	while (std::chrono::steady_clock::now() < deadline) {
		/* RX_READY が立つ前の ATR を読み出すと末尾が FIFO に残るため完成を待つ */
		int ret = WaitCardDataReady(deadline);
		if (ret)
			return ret;

		std::uint8_t chunk[33];
		std::uint8_t chunk_length = static_cast<std::uint8_t>(sizeof(chunk));
		ret = device_->ReadCardData(chunk, chunk_length);
		if (ret)
			return ret;
		if (chunk_length) {
			if (atr.size() + chunk_length > 33)
				return -EBADMSG;
			atr.insert(atr.end(), chunk, chunk + chunk_length);

			ret = ParseAtr(atr, expected_length, parameters);
			if (!ret && expected_length == atr.size())
				return 0;
			if (ret != -EAGAIN)
				return ret;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(CARD_POLL_INTERVAL_MS));
	}

	return -ETIMEDOUT;
}

int SmartCard::WaitCardDataReady(const Deadline &deadline)
{
	/* RX_LENGTH は受信途中でも増えるため、完成通知だけを期限まで待つ */
	while (std::chrono::steady_clock::now() < deadline) {
		bool ready = false;
		int ret = device_->IsCardDataReady(ready);
		if (ret || ready)
			return ret;
		std::this_thread::sleep_for(std::chrono::milliseconds(CARD_POLL_INTERVAL_MS));
	}

	return -ETIMEDOUT;
}

int SmartCard::DiscardPendingCardData()
{
	/*
	 * 追加の待機を挟まず、今読める残余だけを捨てる
	 * カードが応答を送り続ける異常時に抜けられなくなるため回数で区切る
	 */
	for (unsigned int count = 0; count < MAX_DISCARD_CHUNKS; count++) {
		bool ready = false;
		int ret = device_->IsCardDataReady(ready);
		if (ret)
			return ret;
		if (!ready)
			return 0;

		std::uint8_t chunk[CARD_UART_FRAME_MAX_LENGTH];
		std::uint8_t chunk_length = static_cast<std::uint8_t>(sizeof(chunk));
		ret = device_->ReadCardData(chunk, chunk_length);
		if (ret)
			return ret;
		if (!chunk_length)
			return 0;
	}

	/* 捨て切れない量が残る状態は、次の要求へ持ち越さず通信の異常として扱う */
	return -EPROTO;
}

int SmartCard::ParseAtr(const std::vector<std::uint8_t> &atr,
			std::size_t &expected_length, AtrParameters &parameters) const
{
	if (atr.size() < 2)
		return -EAGAIN;
	if (atr[0] != 0x3b && atr[0] != 0x3f)
		return -EBADMSG;

	std::uint8_t interfaces = atr[1] >> 4;
	const std::size_t historical_length = atr[1] & 0x0f;
	std::size_t offset = 2;
	unsigned int group = 1;
	bool has_non_t0_protocol = false;
	bool t1_group = false;
	parameters = AtrParameters();

	while (interfaces) {
		std::uint8_t ta = 0;
		std::uint8_t tb = 0;
		std::uint8_t tc = 0;
		bool has_ta = false;
		bool has_tb = false;
		bool has_tc = false;

		if (interfaces & 0x01) {
			if (offset >= atr.size())
				return -EAGAIN;
			ta = atr[offset++];
			has_ta = true;
		}
		if (interfaces & 0x02) {
			if (offset >= atr.size())
				return -EAGAIN;
			tb = atr[offset++];
			has_tb = true;
		}
		if (interfaces & 0x04) {
			if (offset >= atr.size())
				return -EAGAIN;
			tc = atr[offset++];
			has_tc = true;
		}

		if (group == 1 && has_ta) {
			/* IT930x が生成できる FI=1 の速度だけを受理する */
			switch (ta) {
			case 0x11:
				parameters.baudrate = 9600;
				break;
			case 0x12:
				parameters.baudrate = 19200;
				break;
			case 0x13:
				parameters.baudrate = 38400;
				break;
			default:
				return -EOPNOTSUPP;
			}
		}

		/* T=1 を示す TD の次グループに IFSC と EDC 種別が置かれる */
		if (t1_group && group >= 3) {
			/* TA2 は特定モード、TC2 は T=0 専用なので T=1 値として扱わない */
			if (has_ta && ta >= 1 && ta <= 254)
				parameters.ifsc = ta;
			if (has_tb) {
				/*
				 * TB3 上位4bitの BWI からブロック待機時間を求める
				 * 4MHz 時の基準値89.28ms へ USB/UART の余裕を加えて100ms 単位とし、
				 * 既存カードの500ms 下限と APDU 全体の3秒上限に収める
				 */
				unsigned int bwi = tb >> 4;
				unsigned int block_waiting_time = bwi < 5 ? (100U << bwi) :
					OPERATION_TIMEOUT_MS;
				parameters.block_timeout_ms = std::min<unsigned int>(
					OPERATION_TIMEOUT_MS,
					std::max<unsigned int>(BLOCK_TIMEOUT_MS, block_waiting_time));
			}
			if (has_tc)
				parameters.use_crc = (tc & 0x01) != 0;
		}

		if (interfaces & 0x08) {
			if (offset >= atr.size())
				return -EAGAIN;
			std::uint8_t td = atr[offset++];
			std::uint8_t protocol = td & 0x0f;
			has_non_t0_protocol = has_non_t0_protocol || protocol != 0;
			t1_group = protocol == 1;
			interfaces = td >> 4;
			group++;
		} else {
			interfaces = 0;
		}
	}

	expected_length = offset + historical_length + (has_non_t0_protocol ? 1 : 0);
	if (expected_length > 33)
		return -EBADMSG;
	if (atr.size() < expected_length)
		return -EAGAIN;
	if (atr.size() > expected_length)
		return -EBADMSG;

	/* TCK を含む ATR は T0 から TCK までの XOR が0になる */
	if (has_non_t0_protocol) {
		std::uint8_t checksum = 0;
		for (std::size_t index = 1; index < atr.size(); index++)
			checksum ^= atr[index];
		if (checksum != 0)
			return -EBADMSG;
	}

	(void)group;
	return 0;
}

int SmartCard::InitializeT1(bool resynchronize, std::uint8_t ifsd)
{
	send_sequence_ = 0;
	receive_sequence_ = 0;

	std::uint8_t pcb;
	std::vector<std::uint8_t> data;
	auto deadline = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(OPERATION_TIMEOUT_MS);
	if (resynchronize) {
		int ret = ExchangeBlock(T1_S_BLOCK | T1_S_RESYNCH, nullptr, 0, pcb,
			data, deadline);
		if (ret)
			return ret;
		if (pcb != (T1_S_BLOCK | T1_S_RESPONSE | T1_S_RESYNCH) ||
			!data.empty())
			return -EPROTO;
	}

	/* 最初の I ブロックより前に受信可能な INF の最大長を通知する */
	int ret = ExchangeBlock(T1_S_BLOCK | T1_S_IFS, &ifsd, 1, pcb, data,
		deadline, resynchronize ? MAX_RETRIES : 1);
	if (!resynchronize && (ret == -ETIMEDOUT || ret == -EBADMSG)) {
		/* ACAS が最初の IFS に応答しない場合は RESYNCH 後に IFS を再送する */
		ret = ExchangeBlock(T1_S_BLOCK | T1_S_RESYNCH, nullptr, 0, pcb,
			data, deadline, 1);
		if (ret)
			return ret;
		if (pcb != (T1_S_BLOCK | T1_S_RESPONSE | T1_S_RESYNCH) ||
			!data.empty())
			return -EPROTO;
		ret = ExchangeBlock(T1_S_BLOCK | T1_S_IFS, &ifsd, 1, pcb, data,
			deadline);
	}
	if (ret)
		return ret;
	if (pcb != (T1_S_BLOCK | T1_S_RESPONSE | T1_S_IFS) ||
		data.size() != 1 || data[0] != ifsd)
		return -EPROTO;

	return 0;
}

int SmartCard::SendBlock(std::uint8_t pcb, const std::uint8_t *data,
			 std::size_t length)
{
	if (length > 254 || (length && !data))
		return -EINVAL;

	std::vector<std::uint8_t> frame;
	frame.reserve(3 + length + (use_crc_ ? 2 : 1));
	frame.push_back(T1_NAD);
	frame.push_back(pcb);
	frame.push_back(static_cast<std::uint8_t>(length));
	if (length)
		frame.insert(frame.end(), data, data + length);

	if (use_crc_) {
		std::uint16_t crc = CalculateCrc(frame.data(), frame.size());
		frame.push_back(static_cast<std::uint8_t>(crc >> 8));
		frame.push_back(static_cast<std::uint8_t>(crc & 0xff));
	} else {
		std::uint8_t lrc = 0;
		for (std::uint8_t value : frame)
			lrc ^= value;
		frame.push_back(lrc);
	}

	if (frame.size() > CARD_UART_FRAME_MAX_LENGTH)
		return -EMSGSIZE;
	return device_->WriteCardData(frame.data(), static_cast<std::uint8_t>(frame.size()));
}

int SmartCard::ReceiveBlock(std::uint8_t &pcb, std::vector<std::uint8_t> &data,
			    const Deadline &deadline)
{
	std::vector<std::uint8_t> frame;
	std::size_t expected_length = 0;
	bool read_filled_chunk = false;

	while (std::chrono::steady_clock::now() < deadline) {
		/* RX_LENGTH は受信途中でも増えるため、RX_READY 後に完成フレームを読む */
		int ret = WaitCardDataReady(deadline);
		if (ret)
			return ret;

		std::uint8_t chunk[255];
		std::uint8_t chunk_length = static_cast<std::uint8_t>(sizeof(chunk));
		ret = device_->ReadCardData(chunk, chunk_length);
		if (ret)
			return ret;
		if (chunk_length) {
			read_filled_chunk = chunk_length == sizeof(chunk);
			frame.insert(frame.end(), chunk, chunk + chunk_length);
			if (frame.size() >= 3)
				expected_length = 3 + frame[2] + (use_crc_ ? 2 : 1);
			if (expected_length && frame.size() >= expected_length)
				break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(CARD_POLL_INTERVAL_MS));
	}

	if (!expected_length || frame.size() < expected_length)
		return -ETIMEDOUT;
	/*
	 * 再送前の遅延応答と再送後の応答が同じ UART 読み出しへ連結される場合がある
	 * T=1 は要求に対して次のブロックを自発送信しないため、先頭の完成フレームだけを処理する
	 */
	/*
	 * 読み出しがバッファを埋め切った場合は、完成フレームがちょうど上限の長さでも
	 * 続きが FIFO に残っていることがあるため、余りの有無によらず捨てる
	 */
	if (frame.size() > expected_length || read_filled_chunk) {
		/*
		 * 遅延した旧フレームが新フレームと連結された極端な場合、切り詰めで
		 * 新フレームの一部を捨てることになる
		 * 採用した旧フレームがまだ処理していない応答であれば、期待する N(S) と
		 * 一致するため、再送された正当な応答として受理される
		 * 既に処理して受信連番が進んだ後の応答であれば N(S) が食い違い、
		 * TransmitInitialized() の検査で -EPROTO となってセッションを作り直す
		 * どちらの場合も、古い応答を別の応答として取り違える経路はない
		 */
		frame.resize(expected_length);
		/*
		 * 連結された応答が1回の ReadCardData() で読み切れないと FIFO へ残りが留まり、
		 * 次のブロック受信の先頭へ混ざって連番不一致を起こす
		 */
		int discard_ret = DiscardPendingCardData();

		if (discard_ret)
			return discard_ret;
	}
	if (frame[0] != T1_NAD)
		return -EPROTO;

	if (use_crc_) {
		std::uint16_t expected_crc = CalculateCrc(frame.data(), frame.size() - 2);
		std::uint16_t received_crc =
			(static_cast<std::uint16_t>(frame[frame.size() - 2]) << 8) |
			frame.back();
		if (expected_crc != received_crc)
			return -EBADMSG;
	} else {
		std::uint8_t lrc = 0;
		for (std::uint8_t value : frame)
			lrc ^= value;
		if (lrc != 0)
			return -EBADMSG;
	}

	pcb = frame[1];
	data.assign(frame.begin() + 3, frame.begin() + 3 + frame[2]);
	return 0;
}

int SmartCard::ExchangeBlock(std::uint8_t pcb, const std::uint8_t *send_data,
			     std::size_t send_length, std::uint8_t &recv_pcb,
			     std::vector<std::uint8_t> &recv_data,
			     const Deadline &deadline, unsigned int max_retries)
{
	for (unsigned int retry = 0; retry < max_retries; retry++) {
		if (std::chrono::steady_clock::now() >= deadline)
			return -ETIMEDOUT;
		int ret = SendBlock(pcb, send_data, send_length);
		if (ret)
			return ret;

		/* 各再送をカード指定の待機時間で区切り、APDU 全体の期限は延長しない */
		auto block_deadline = std::min<Deadline>(deadline,
			std::chrono::steady_clock::now() +
			std::chrono::milliseconds(block_timeout_ms_));
		ret = ReceiveBlock(recv_pcb, recv_data, block_deadline);
		if (!ret) {
			/* WTX へ応答しても APDU 開始時の期限は延長しない */
			while (recv_pcb == (T1_S_BLOCK | T1_S_WTX)) {
				if (recv_data.size() != 1 || recv_data[0] == 0)
					return -EPROTO;
				std::uint8_t multiplier = recv_data[0];
				ret = SendBlock(T1_S_BLOCK | T1_S_RESPONSE | T1_S_WTX,
						&multiplier, 1);
				if (ret)
					return ret;
				ret = ReceiveBlock(recv_pcb, recv_data, deadline);
				if (ret)
					break;
			}
			if (!ret)
				return 0;
		}
		if (ret != -ETIMEDOUT && ret != -EBADMSG)
			return ret;
	}

	return -ETIMEDOUT;
}

int SmartCard::Transmit(const std::uint8_t *send_buf, std::size_t send_len,
			std::uint8_t *recv_buf, std::size_t &recv_len)
{
	if (!open_)
		return SMART_CARD_NO_MEDIUM;

	/* 接触不良や抜去を APDU 送信前に検出し、壊れたセッションを即座に破棄する */
	bool detected = false;
	int status_ret = device_->DetectCard(detected);
	if (status_ret) {
		InvalidateSession();
		return status_ret;
	}
	if (!detected) {
		InvalidateSession();
		return SMART_CARD_NO_MEDIUM;
	}
	present_ = true;
	if (!initialized_) {
		std::vector<std::uint8_t> atr;
		int reset_ret = Reset(atr);
		if (reset_ret)
			return reset_ret;
	}

	/* WTX やブロック再送が続いても1回の APDU を有限時間で打ち切る */
	auto deadline = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(OPERATION_TIMEOUT_MS);
	/*
	 * 通信失敗後は次の APDU でカードを再初期化し、壊れた連番を残さない
	 * 送受信中の例外でも同じ後始末が要るため、破棄はスコープの終わりに任せる
	 */
	bool succeeded = false;
	struct SessionGuard final {
		SmartCard &card;
		const bool &succeeded;
		~SessionGuard()
		{
			if (!succeeded)
				card.InvalidateSession();
		}
	} guard{ *this, succeeded };

	int ret = TransmitInitialized(send_buf, send_len, recv_buf, recv_len, deadline);
	succeeded = !ret;
	return ret;
}

int SmartCard::TransmitInitialized(const std::uint8_t *send_buf,
				    std::size_t send_len, std::uint8_t *recv_buf,
				    std::size_t &recv_len, const Deadline &deadline)
{
	if (!send_buf || !send_len || !recv_buf)
		return -EINVAL;

	const std::size_t recv_capacity = recv_len;
	recv_len = 0;
	std::size_t send_offset = 0;
	std::uint8_t last_pcb = 0;
	std::vector<std::uint8_t> last_data;
	/* カードの IFSC が大きくても1回の UART 送信は255バイトを超えられない */
	const std::size_t max_inf_length = CARD_UART_FRAME_MAX_LENGTH -
		3 - (use_crc_ ? 2 : 1);

	while (send_offset < send_len) {
		std::size_t chunk_length = std::min<std::size_t>(
			std::min<std::size_t>(card_ifsc_, max_inf_length),
			send_len - send_offset);
		bool has_more = send_offset + chunk_length < send_len;
		std::uint8_t pcb = T1_I_BLOCK |
			(send_sequence_ ? T1_I_SEQUENCE : 0) |
			(has_more ? T1_I_CHAIN : 0);
		bool accepted = false;
		for (unsigned int retry = 0; retry < MAX_RETRIES; retry++) {
			int ret = ExchangeBlock(pcb, send_buf + send_offset, chunk_length,
						last_pcb, last_data, deadline);
			if (ret)
				return ret;

			if ((last_pcb & 0xc0) == T1_R_BLOCK) {
				bool requested_sequence = (last_pcb & T1_R_SEQUENCE) != 0;
				bool current_sequence = send_sequence_ != 0;

				/* 現在の N(S) を要求された場合だけ同じ I ブロックを再送する */
				if (requested_sequence == current_sequence)
					continue;

				/* 次の N(S) はカードが I ブロックを受理済みであることを示す */
				accepted = true;
				break;
			}

			if ((last_pcb & 0x80) == T1_I_BLOCK && !has_more) {
				accepted = true;
				break;
			}
			return -EPROTO;
		}
		if (!accepted)
			return -EIO;

		/*
		 * 最後の I ブロックに対するカード応答を取りこぼすと、再送した
		 * I ブロックには受理済みを示す R ブロックが返る
		 * この場合はカード側 I ブロックの再送を要求して応答を回収する
		 */
		if (!has_more && (last_pcb & 0xc0) == T1_R_BLOCK) {
			bool response_received = false;
			for (unsigned int retry = 0; retry < MAX_RETRIES; retry++) {
				std::uint8_t r_pcb = T1_R_BLOCK |
					(receive_sequence_ ? T1_R_SEQUENCE : 0);
				int ret = ExchangeBlock(r_pcb, nullptr, 0, last_pcb, last_data,
					deadline);
				if (ret)
					return ret;
				if ((last_pcb & 0x80) == T1_I_BLOCK) {
					response_received = true;
					break;
				}
				if ((last_pcb & 0xc0) != T1_R_BLOCK)
					return -EPROTO;
			}
			if (!response_received)
				return -EIO;
		}

		send_sequence_ ^= 1;
		send_offset += chunk_length;
	}

	while (true) {
		/* カードからの連結 I ブロックを順番に応答バッファへ追加する */
		if (((last_pcb & T1_I_SEQUENCE) != 0) != (receive_sequence_ != 0))
			return -EPROTO;
		if (recv_len + last_data.size() > recv_capacity)
			return -ENOBUFS;
		memcpy(recv_buf + recv_len, last_data.data(), last_data.size());
		recv_len += last_data.size();
		receive_sequence_ ^= 1;

		if (!(last_pcb & T1_I_CHAIN))
			return 0;

		std::uint8_t r_pcb = T1_R_BLOCK |
			(receive_sequence_ ? T1_R_SEQUENCE : 0);
		int ret = ExchangeBlock(r_pcb, nullptr, 0, last_pcb, last_data,
			deadline);
		if (ret)
			return ret;
		if ((last_pcb & 0x80) != T1_I_BLOCK)
			return -EPROTO;
	}
}

void SmartCard::InvalidateSession() noexcept
{
	present_ = false;
	initialized_ = false;
	use_crc_ = false;
	card_ifsc_ = 32;
	block_timeout_ms_ = BLOCK_TIMEOUT_MS;
	send_sequence_ = 0;
	receive_sequence_ = 0;
	atr_.clear();
}

std::uint16_t SmartCard::CalculateCrc(const std::uint8_t *data,
				      std::size_t length) const
{
	std::uint16_t crc = 0xffff;

	for (std::size_t index = 0; index < length; index++) {
		std::uint16_t value = static_cast<std::uint16_t>(data[index]) << 8;
		for (unsigned int bit = 0; bit < 8; bit++) {
			if ((crc ^ value) & 0x8000)
				crc = static_cast<std::uint16_t>((crc << 1) ^ 0x1021);
			else
				crc = static_cast<std::uint16_t>(crc << 1);
			value = static_cast<std::uint16_t>(value << 1);
		}
	}

	return crc;
}

} // namespace px4
