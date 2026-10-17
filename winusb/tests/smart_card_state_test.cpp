#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <new>
#include <memory>
#include <vector>

#include "smart_card.hpp"

namespace {

class MockCardDevice final : public px4::CardDevice {
public:
	int OpenCard() override
	{
		is_open = true;
		return 0;
	}

	void CloseCard() override
	{
		is_open = false;
	}

	int DetectCard(bool &detected) override
	{
		if (detect_error) {
			int error = detect_error;
			detect_error = 0;
			return error;
		}
		detected = is_present;
		return 0;
	}

	int ResetCard() override
	{
		if (!is_present)
			return -ENODEV;
		reset_count++;
		read_queue.clear();
		/* セッション再構築でカード側の N(S) も初期値へ戻る */
		response_sequence = 0;
		/* 前のセッションの応答を、再送要求で送り直してしまわないよう捨てる */
		last_response_pcb = 0;
		last_response.clear();
		final_response_lost = false;
		if (malformed_atr_resets) {
			malformed_atr_resets--;
			read_queue.insert(read_queue.end(), malformed_atr_bytes.begin(),
				malformed_atr_bytes.end());
		} else {
			read_queue.insert(read_queue.end(), atr_bytes.begin(), atr_bytes.end());
		}
		return 0;
	}

	int SetCardBaudrate(std::uint32_t value) override
	{
		baudrate = value;
		return 0;
	}

	int IsCardDataReady(bool &ready) override
	{
		if (ready_delay_checks) {
			ready_delay_checks--;
			ready = false;
			read_allowed = false;
			return 0;
		}
		ready = !read_queue.empty();
		read_allowed = ready;
		return 0;
	}

	int ReadCardData(std::uint8_t *buffer, std::uint8_t &length) override
	{
		if (!read_allowed)
			read_called_before_ready = true;
		read_allowed = false;
		std::size_t copy_length = std::min<std::size_t>(length, read_queue.size());
		for (std::size_t index = 0; index < copy_length; index++) {
			buffer[index] = read_queue.front();
			read_queue.pop_front();
		}
		length = static_cast<std::uint8_t>(copy_length);
		return 0;
	}

	int WriteCardData(const std::uint8_t *buffer, std::uint8_t length) override
	{
		if (throw_next_apdu) {
			throw_next_apdu = false;
			throw std::bad_alloc();
		}
		if (length < 4)
			return -EINVAL;
		const std::uint8_t pcb = buffer[1];
		written_pcbs.push_back(pcb);
		const std::uint8_t data_length = buffer[2];
		if (pcb == 0xc1 && data_length == 1)
			ifs_values.push_back(buffer[3]);
		std::size_t expected_length = static_cast<std::size_t>(data_length) +
			(expected_crc ? 5 : 4);
		if (expected_length != length || !ValidateEdc(buffer, length))
			return -EINVAL;
		if (drop_first_ifs && pcb == 0xc1) {
			drop_first_ifs = false;
			return 0;
		}

		/* 初期化 S ブロックは要求データをそのまま応答する */
		if ((pcb & 0xc0) == 0xc0) {
			if (endless_wtx && pcb == 0xe3) {
				const std::uint8_t multiplier = 1;
				QueueBlock(0xc3, &multiplier, 1);
				return 0;
			}
			QueueBlock(static_cast<std::uint8_t>(pcb | 0x20),
				buffer + 3, data_length);
			return 0;
		}

		if (fail_next_apdu) {
			fail_next_apdu = false;
			return -EIO;
		}
		if (endless_wtx) {
			const std::uint8_t multiplier = 1;
			QueueBlock(0xc3, &multiplier, 1);
			return 0;
		}
		/* 連結 I ブロックを受理したら次の送信連番を R ブロックで要求する */
		if ((pcb & 0x80) == 0) {
			apdu_block_lengths.push_back(data_length);
			if (pcb & 0x20) {
				const std::uint8_t next_sequence = (pcb & 0x40) ? 0 : 0x10;
				QueueBlock(static_cast<std::uint8_t>(0x80 | next_sequence),
					buffer, 0);
				return 0;
			}
		}
		/*
		 * 応答を取りこぼしたホストは R ブロックで再送を要求する
		 * 要求された N(R) が保留中の応答の N(S) と一致するときだけ、連番を進めず
		 * 直前と同じ I ブロックを送り直す
		 * 一致しない要求は再送とみなさず、連番の食い違いをホスト側に検出させる
		 */
		if ((pcb & 0xc0) == 0x80 && !last_response.empty()) {
			const std::uint8_t expected_sequence =
				(last_response_pcb & 0x40) ? 0x10 : 0x00;

			if ((pcb & 0x10) == expected_sequence) {
				cached_resend_count++;
				QueueBlock(last_response_pcb, last_response.data(),
					last_response.size());
				return 0;
			}
		}
		/*
		 * 応答を落とした後に同じ I ブロックが再送されてきた状態
		 * カードは受理済みなので連番を進めず、受理を示す R ブロックだけを返す
		 */
		if (final_response_lost) {
			final_response_lost = false;
			const std::uint8_t next_sequence = (pcb & 0x40) ? 0 : 0x10;
			QueueBlock(static_cast<std::uint8_t>(0x80 | next_sequence),
				buffer, 0);
			return 0;
		}
		/* I ブロック応答はカード側の N(S) を交互に進める */
		const std::uint8_t response_pcb = response_sequence ? 0x40 : 0x00;
		response_sequence ^= 1;
		if (lose_final_response) {
			lose_final_response = false;
			final_response_lost = true;
			/*
			 * カードは I ブロックを受理して応答を作ったが、それが届かなかった状態を作る
			 * 何も返さないため、ホストは待機時間を使い切って I ブロックを再送する
			 */
			last_response_pcb = response_pcb;
			last_response = { 0x90, 0x00 };
			return 0;
		}
		if (large_duplicate_response) {
			large_duplicate_response = false;
			/* 2フレームの合計が1回の読み出し上限255バイトを超える長さにする */
			const std::vector<std::uint8_t> large(200, 0xa5);
			QueueBlock(response_pcb, large.data(), large.size());
			QueueBlock(response_pcb, large.data(), large.size());
			return 0;
		}
		if (max_duplicate_response) {
			max_duplicate_response = false;
			/* 1フレームが読み出し上限とちょうど同じ255バイトになる長さにする */
			const std::vector<std::uint8_t> max_inf(251, 0x5a);
			QueueBlock(response_pcb, max_inf.data(), max_inf.size());
			QueueBlock(response_pcb, max_inf.data(), max_inf.size());
			return 0;
		}
		const std::uint8_t response[] = { 0x90, 0x00 };
		last_response_pcb = response_pcb;
		last_response.assign(response, response + sizeof(response));
		QueueBlock(response_pcb, response, sizeof(response));
		if (duplicate_next_response) {
			duplicate_next_response = false;
			QueueBlock(response_pcb, response, sizeof(response));
		}
		if (delay_next_response) {
			delay_next_response = false;
			ready_delay_checks = 3;
		}
		return 0;
	}

	void QueueBytes(std::initializer_list<std::uint8_t> bytes)
	{
		read_queue.insert(read_queue.end(), bytes.begin(), bytes.end());
	}

	void QueueBlock(std::uint8_t pcb, const std::uint8_t *data,
		std::size_t data_length)
	{
		std::vector<std::uint8_t> frame = {
			0x00,
			pcb,
			static_cast<std::uint8_t>(data_length),
		};
		frame.insert(frame.end(), data, data + data_length);
		if (expected_crc) {
			std::uint16_t crc = CalculateCrc(frame.data(), frame.size());
			frame.push_back(static_cast<std::uint8_t>(crc >> 8));
			frame.push_back(static_cast<std::uint8_t>(crc & 0xff));
		} else {
			std::uint8_t lrc = 0;
			for (std::uint8_t value : frame)
				lrc ^= value;
			frame.push_back(lrc);
		}
		read_queue.insert(read_queue.end(), frame.begin(), frame.end());
	}

	bool ValidateEdc(const std::uint8_t *frame, std::size_t length) const
	{
		if (expected_crc) {
			std::uint16_t crc = CalculateCrc(frame, length - 2);
			return frame[length - 2] == static_cast<std::uint8_t>(crc >> 8) &&
				frame[length - 1] == static_cast<std::uint8_t>(crc & 0xff);
		}
		std::uint8_t lrc = 0;
		for (std::size_t index = 0; index < length; index++)
			lrc ^= frame[index];
		return lrc == 0;
	}

	static std::uint16_t CalculateCrc(const std::uint8_t *data, std::size_t length)
	{
		std::uint16_t crc = 0xffff;
		for (std::size_t index = 0; index < length; index++) {
			crc ^= static_cast<std::uint16_t>(data[index]) << 8;
			for (unsigned int bit = 0; bit < 8; bit++)
				crc = (crc & 0x8000) ?
					static_cast<std::uint16_t>((crc << 1) ^ 0x1021) :
					static_cast<std::uint16_t>(crc << 1);
		}
		return crc;
	}

	bool is_open = false;
	bool is_present = false;
	bool fail_next_apdu = false;
	bool throw_next_apdu = false;
	bool endless_wtx = false;
	bool delay_next_response = false;
	bool duplicate_next_response = false;
	bool large_duplicate_response = false;
	bool max_duplicate_response = false;
	bool lose_final_response = false;
	bool final_response_lost = false;
	unsigned int cached_resend_count = 0;
	std::uint8_t response_sequence = 0;
	std::uint8_t last_response_pcb = 0;
	std::vector<std::uint8_t> last_response;
	bool drop_first_ifs = false;
	bool read_called_before_ready = false;
	bool read_allowed = false;
	unsigned int ready_delay_checks = 0;
	int detect_error = 0;
	unsigned int reset_count = 0;
	unsigned int malformed_atr_resets = 0;
	std::vector<std::uint8_t> malformed_atr_bytes = { 0xfc, 0xff };
	std::vector<std::uint8_t> atr_bytes = {
		0x3b, 0xf0, 0x12, 0x00, 0xff, 0x91, 0x81,
		0xb1, 0x7c, 0x45, 0x1f, 0x01, 0x9b,
	};
	std::uint32_t baudrate = 9600;
	bool expected_crc = false;
	std::vector<std::uint8_t> written_pcbs;
	std::vector<std::uint8_t> ifs_values;
	std::vector<std::uint8_t> apdu_block_lengths;
	std::deque<std::uint8_t> read_queue;
};

bool Check(bool condition, const char *message)
{
	if (condition)
		return true;
	std::fprintf(stderr, "%s\n", message);
	return false;
}

} // namespace

int main()
{
	auto device = std::make_shared<MockCardDevice>();
	px4::SmartCard card(device);
	bool succeeded = Check(card.Open() == 0 && device->is_open,
		"Opening an empty reader failed.");

	bool present = true;
	bool initialized = true;
	std::vector<std::uint8_t> atr;
	succeeded = Check(card.GetStatus(present, initialized, atr) == 0 &&
		!present && !initialized && atr.empty(),
		"An empty reader returned stale card state.") && succeeded;

	/* 挿入検出時に ATR と T=1 を初期化する */
	device->is_present = true;
	succeeded = Check(card.GetStatus(present, initialized, atr) == 0 &&
		present && initialized && atr.size() == 13 && device->reset_count == 1 &&
		device->baudrate == 19200 &&
		device->written_pcbs.size() == 2 && device->written_pcbs[0] == 0xc0 &&
		device->written_pcbs[1] == 0xc1 && device->ifs_values.size() == 1 &&
		device->ifs_values[0] == 251,
		"Card insertion did not initialize the session.") && succeeded;

	const std::uint8_t apdu[] = { 0x90, 0x30, 0x00, 0x00, 0x00 };
	std::uint8_t response[16] = {};
	std::size_t response_length = sizeof(response);
	/* UART_RX_READY が立つまで途中フレームを読み出さない */
	device->delay_next_response = true;
	/* 同じ UART 読み出しへ連結された重複応答は先頭フレームの処理後に残さない */
	device->duplicate_next_response = true;
	device->read_called_before_ready = false;
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && response_length == 2 &&
		!device->read_called_before_ready && device->read_queue.empty(),
		"Initial APDU transmission failed.") && succeeded;

	/* 1回の読み出し上限を超える重複応答でも、先頭フレームだけを処理して残りを FIFO に残さない */
	device->large_duplicate_response = true;
	std::uint8_t large_response[256] = {};
	std::size_t large_response_length = sizeof(large_response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), large_response,
		large_response_length) == 0 && large_response_length == 200 &&
		device->read_queue.empty(),
		"A duplicated response across the read limit was not discarded.") && succeeded;

	/* FIFO の残留を持ち越さず、続く APDU も同じセッションで成功する */
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && response_length == 2,
		"The following APDU failed after a duplicated long response.") && succeeded;

	/* 完成フレームが読み出し上限と同じ長さでも、後続の重複を残さない */
	device->max_duplicate_response = true;
	large_response_length = sizeof(large_response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), large_response,
		large_response_length) == 0 && large_response_length == 251 &&
		device->read_queue.empty(),
		"A duplicated response at the read limit was not discarded.") && succeeded;

	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && response_length == 2,
		"The following APDU failed after a maximum length response.") && succeeded;

	/* 抜去後は古い ATR と連番を破棄する */
	device->is_present = false;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == px4::SMART_CARD_NO_MEDIUM,
		"Card removal was not reported.") && succeeded;

	/* 再挿入後の最初の APDU でセッションを自動再構築する */
	device->is_present = true;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && device->reset_count == 2,
		"Card reinsertion did not recover automatically.") && succeeded;

	/* 一時的な通信失敗も次の APDU まで壊れた連番を持ち越さない */
	device->fail_next_apdu = true;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == -EIO,
		"Injected transport failure was not returned.") && succeeded;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && device->reset_count == 3,
		"Transport failure did not recover on the next APDU.") && succeeded;

	/* 送受信中に例外が飛んでもセッションを破棄し、壊れた連番を残さない */
	device->throw_next_apdu = true;
	response_length = sizeof(response);
	bool threw = false;
	try {
		card.Transmit(apdu, sizeof(apdu), response, response_length);
	} catch (const std::bad_alloc &) {
		threw = true;
	}
	succeeded = Check(threw, "An injected exception was not propagated.") && succeeded;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && device->reset_count == 4,
		"An exception did not invalidate the session.") && succeeded;

	/* 接触不良による検出エラーも例外や状態破損へ変えない */
	device->detect_error = -EIO;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == -EIO,
		"Card detection failure was not returned.") && succeeded;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && device->reset_count == 5,
		"Card detection failure did not recover.") && succeeded;

	/* 正常な WTX が繰り返されても APDU 全体の期限を延長しない */
	device->endless_wtx = true;
	auto wtx_started = std::chrono::steady_clock::now();
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == -ETIMEDOUT &&
		std::chrono::steady_clock::now() - wtx_started < std::chrono::seconds(5),
		"Repeated WTX requests extended the APDU indefinitely.") && succeeded;
	device->endless_wtx = false;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && device->reset_count == 6,
		"WTX timeout did not invalidate the session.") && succeeded;

	/*
	 * 最後の I ブロックへの応答を取りこぼすと、待機時間の経過後に I ブロックが再送され、
	 * 受理済みを示す R ブロックが返る
	 * ここからカード側 I ブロックの再送を要求して応答を回収できることを確かめる
	 * 通常の応答経路で成功していないことを、再送回数でも確かめる
	 */
	device->lose_final_response = true;
	device->cached_resend_count = 0;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && response_length == 2 &&
		device->cached_resend_count == 1 && device->reset_count == 6,
		"A lost final I-block response was not recovered.") && succeeded;

	/*
	 * 応答を読み終えた後から遅延した重複応答が届くと、読み捨ての対象にならず
	 * 次の APDU の先頭で読まれるが、直前の応答で受信連番が進んでいるため
	 * 必ず連番不一致で失敗し、古い応答が正しい応答として通る経路はない
	 * ここではその失敗が1回で済み、次の APDU で再初期化して回復することを確かめる
	 */
	const std::uint8_t stale_response[] = { 0x90, 0x00 };
	device->QueueBlock(device->response_sequence ? 0x00 : 0x40,
		stale_response, sizeof(stale_response));
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == -EPROTO,
		"A stale delayed response was not rejected.") && succeeded;
	response_length = sizeof(response);
	succeeded = Check(card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && device->reset_count == 7,
		"A stale delayed response did not recover on the next APDU.") && succeeded;

	/* UART の残留バイトで ATR が壊れた場合だけ、カード全体を1回再初期化する */
	auto retry_device = std::make_shared<MockCardDevice>();
	retry_device->is_present = true;
	retry_device->malformed_atr_resets = 1;
	/* CRC 指定まで解析してから TCK で失敗し、次の LRC 用 ATR へ設定を残さないことも確認する */
	retry_device->malformed_atr_bytes = {
		0x3b, 0x80, 0x91, 0x01, 0x51, 0x40, 0x01, 0x01,
	};
	px4::SmartCard retry_card(retry_device);
	succeeded = Check(retry_card.Open() == 0 && retry_device->reset_count == 2,
		"Malformed ATR did not recover after one card reset.") && succeeded;
	retry_card.Close();

	/* TD2 が T=1 を示した後の TA3 / TC3 だけから IFSC と CRC を取得する */
	auto crc_device = std::make_shared<MockCardDevice>();
	crc_device->atr_bytes = { 0x3b, 0x80, 0x91, 0x01, 0x51, 0x40, 0x01, 0x00 };
	crc_device->expected_crc = true;
	crc_device->is_present = true;
	px4::SmartCard crc_card(crc_device);
	bool crc_present = false;
	bool crc_initialized = false;
	std::vector<std::uint8_t> crc_atr;
	succeeded = Check(crc_card.Open() == 0 &&
		crc_card.GetStatus(crc_present, crc_initialized, crc_atr) == 0 &&
		crc_present && crc_initialized && crc_atr.size() == 8,
		"TA3 / TC3 CRC parameters were not applied.") && succeeded;
	crc_card.Close();

	/* ACAS の TA1=13 は PPS なしで IT930x の38400bps 設定へ切り替える */
	auto acas_device = std::make_shared<MockCardDevice>();
	acas_device->atr_bytes = {
		0x3b, 0xf0, 0x13, 0x00, 0xff, 0x91, 0x81,
		0xb1, 0xfe, 0x46, 0x1f, 0x03, 0x19,
	};
	acas_device->is_present = true;
	acas_device->drop_first_ifs = true;
	px4::SmartCard acas_card(acas_device);
	succeeded = Check(acas_card.Open() == 0 &&
		acas_device->baudrate == 38400 &&
		acas_device->written_pcbs.size() == 3 &&
		acas_device->written_pcbs[0] == 0xc1 &&
		acas_device->written_pcbs[1] == 0xc0 &&
		acas_device->written_pcbs[2] == 0xc1 &&
		acas_device->ifs_values.size() == 2 &&
		acas_device->ifs_values[0] == 254 && acas_device->ifs_values[1] == 254,
		"ACAS IFS timeout did not recover through RESYNCH and IFS retry.") &&
		succeeded;

	/* TB3 の BWI=4 は500ms を超える正当なブロック待機時間を許容する */
	acas_device->ready_delay_checks = 120;
	response_length = sizeof(response);
	succeeded = Check(acas_card.Transmit(apdu, sizeof(apdu), response,
		response_length) == 0 && response_length == 2,
		"ACAS BWI was not applied to the T=1 block timeout.") && succeeded;
	acas_card.Close();

	/* ACAS の IFSC=254 でも UART 上限に合わせて I ブロックを連結送信する */
	auto long_apdu_device = std::make_shared<MockCardDevice>();
	long_apdu_device->atr_bytes = acas_device->atr_bytes;
	long_apdu_device->is_present = true;
	px4::SmartCard long_apdu_card(long_apdu_device);
	std::vector<std::uint8_t> long_apdu(254, 0);
	response_length = sizeof(response);
	succeeded = Check(long_apdu_card.Open() == 0 &&
		long_apdu_card.Transmit(long_apdu.data(), long_apdu.size(), response,
			response_length) == 0 &&
		response_length == 2 &&
		long_apdu_device->apdu_block_lengths.size() == 2 &&
		long_apdu_device->apdu_block_lengths[0] == 251 &&
		long_apdu_device->apdu_block_lengths[1] == 3,
		"ACAS APDU was not split at the UART frame limit.") && succeeded;
	long_apdu_card.Close();

	/* IT930x で生成できない TA1 は誤った通信速度へ丸めず明示的に拒否する */
	auto unsupported_device = std::make_shared<MockCardDevice>();
	unsupported_device->atr_bytes = {
		0x3b, 0xf0, 0x14, 0x00, 0xff, 0x91, 0x81,
		0xb1, 0x7c, 0x45, 0x1f, 0x01, 0x9d,
	};
	unsupported_device->is_present = true;
	px4::SmartCard unsupported_card(unsupported_device);
	succeeded = Check(unsupported_card.Open() == -EOPNOTSUPP &&
		!unsupported_device->is_open,
		"An unsupported TA1 value was accepted.") && succeeded;

	card.Close();
	succeeded = Check(!device->is_open, "Closing the reader failed.") && succeeded;
	std::printf("smart_card_state_test: %s\n", succeeded ? "passed" : "failed");
	return succeeded ? 0 : 1;
}
