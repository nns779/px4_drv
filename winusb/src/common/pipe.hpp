// pipe.hpp

#pragma once

#include <string>
#include <stdexcept>
#include <system_error>

#include <windows.h>

namespace px4 {

class Pipe {
public:
	explicit Pipe(HANDLE handle) noexcept;	// for compatibility
	virtual ~Pipe();

	// cannot copy
	Pipe(const Pipe &) = delete;
	Pipe& operator=(const Pipe &) = delete;

	// cannot move
	Pipe(Pipe &&) = delete;
	Pipe& operator=(Pipe &&) = delete;

	bool Read(void *buf, std::size_t size, std::size_t &return_size) noexcept;
	bool Read(void *buf, std::size_t size, std::size_t &return_size, HANDLE cancel_event) noexcept;
	bool Write(const void *buf, std::size_t size, std::size_t &return_size) noexcept;
	bool Write(const void *buf, std::size_t size, std::size_t &return_size, HANDLE cancel_event) noexcept;
	bool Call(void *buf, std::size_t size) noexcept;
	bool Call(const void *buf_in, std::size_t size_in, void *buf_out, std::size_t size_out) noexcept;

	const std::error_condition& GetError() const noexcept { return error_; }

protected:
	Pipe() noexcept : Pipe(nullptr) {}
	
	bool IsConnected() const noexcept { return (handle_ && handle_ != INVALID_HANDLE_VALUE); }
	void SetHandle(HANDLE handle) noexcept { handle_ = handle; }

	std::error_condition error_;
	HANDLE handle_;
	HANDLE ol_event_[2];	// [0]: read, [1]: write
};

class PipeError : public std::runtime_error {
public:
	explicit PipeError(const std::string& what_arg) : runtime_error(what_arg.c_str()) {};
};

} // namespace px4
