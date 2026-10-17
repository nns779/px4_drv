#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "card_device.hpp"

namespace px4 {

constexpr int SMART_CARD_NO_MEDIUM = -10001;

class SmartCard final {
public:
	explicit SmartCard(std::shared_ptr<CardDevice> device) noexcept;
	~SmartCard();

	SmartCard(const SmartCard &) = delete;
	SmartCard& operator=(const SmartCard &) = delete;
	SmartCard(SmartCard &&) = delete;
	SmartCard& operator=(SmartCard &&) = delete;

	int Open();
	void Close() noexcept;
	int GetStatus(bool &present, bool &initialized, std::vector<std::uint8_t> &atr);
	int Reset(std::vector<std::uint8_t> &atr);
	int Transmit(const std::uint8_t *send_buf, std::size_t send_len,
		     std::uint8_t *recv_buf, std::size_t &recv_len);

private:
	using Deadline = std::chrono::steady_clock::time_point;

	struct AtrParameters final {
		std::uint32_t baudrate = 9600;
		std::uint8_t ifsc = 32;
		bool use_crc = false;
		unsigned int block_timeout_ms = 500;
	};

	int ReadAtr(std::vector<std::uint8_t> &atr, AtrParameters &parameters);
	int WaitCardDataReady(const Deadline &deadline);
	int DiscardPendingCardData();
	int ParseAtr(const std::vector<std::uint8_t> &atr,
		     std::size_t &expected_length, AtrParameters &parameters) const;
	int InitializeT1(bool resynchronize, std::uint8_t ifsd);
	int SendBlock(std::uint8_t pcb, const std::uint8_t *data, std::size_t length);
	int ReceiveBlock(std::uint8_t &pcb, std::vector<std::uint8_t> &data,
			 const Deadline &deadline);
	int ExchangeBlock(std::uint8_t pcb, const std::uint8_t *send_data,
			  std::size_t send_length, std::uint8_t &recv_pcb,
			  std::vector<std::uint8_t> &recv_data,
			  const Deadline &deadline, unsigned int max_retries = 3);
	int TransmitInitialized(const std::uint8_t *send_buf, std::size_t send_len,
				std::uint8_t *recv_buf, std::size_t &recv_len,
				const Deadline &deadline);
	std::uint16_t CalculateCrc(const std::uint8_t *data, std::size_t length) const;
	void InvalidateSession() noexcept;

	std::shared_ptr<CardDevice> device_;
	bool open_;
	bool present_;
	bool initialized_;
	bool use_crc_;
	std::uint8_t card_ifsc_;
	unsigned int block_timeout_ms_;
	std::uint8_t send_sequence_;
	std::uint8_t receive_sequence_;
	std::vector<std::uint8_t> atr_;
};

} // namespace px4
