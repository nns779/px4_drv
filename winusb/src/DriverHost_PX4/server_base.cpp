// server_base.cpp

#include "server_base.hpp"

namespace px4 {

ServerBase::ServerBase(const std::wstring &pipe_name, px4::ReceiverManager &receiver_manager) noexcept
	: pipe_name_(pipe_name),
	receiver_manager_(receiver_manager),
	mtx_(),
	quit_event_(nullptr)
{
	pipe_config_ = { 0 };
}

ServerBase::~ServerBase()
{
	Stop();
}

bool ServerBase::Start() noexcept
{
	if (th_) {
		error_.assign(EINVAL, std::generic_category());
		return false;
	}

	if (!quit_event_) {
		quit_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!quit_event_) {
			error_.assign(GetLastError(), std::system_category());
			return false;
		}
	}

	HANDLE ready_event;

	ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!ready_event) {
		CloseHandle(quit_event_);
		quit_event_ = nullptr;

		error_.assign(GetLastError(), std::system_category());
		return false;
	}

	try {
		th_.reset(new std::thread(&px4::ServerBase::Worker, this, ready_event));
	} catch (const std::system_error &e) {
		CloseHandle(ready_event);
		CloseHandle(quit_event_);
		quit_event_ = nullptr;

		error_.assign(e.code().value(), e.code().category());
		return false;
	}

	WaitForSingleObject(ready_event, INFINITE);
	CloseHandle(ready_event);

	return true;
}

bool ServerBase::Stop() noexcept
{
	if (!th_) {
		error_.assign(EINVAL, std::generic_category());
		return false;
	}

	SetEvent(quit_event_);
	try {
		th_->join();
	} catch (...) {}

	th_.reset();

	try {
		std::unique_lock<std::mutex> lock(mtx_);

		while (conns_.size())
			cond_.wait(lock);

		lock.unlock();
	} catch (...) {}

	CloseHandle(quit_event_);
	quit_event_ = nullptr;

	return true;
}

std::size_t ServerBase::GetActiveConnectionCount() const noexcept
{
	std::lock_guard<std::mutex> lock(mtx_);

	return conns_.size();
}

void ServerBase::RemoveConnection(Connection *conn, bool destruct) noexcept
{
	bool empty = false;
	
	try {
		std::lock_guard<std::mutex> lock(mtx_);

		for (auto it = conns_.begin(); it != conns_.end(); ++it) {
			if (it->get() == conn) {
				if (!destruct)
					it->release();

				conns_.erase(it);
				break;
			}
		}

		empty = !conns_.size();
	} catch (...) {}

	if (empty)
		cond_.notify_all();
}

void ServerBase::Worker(HANDLE ready_event) noexcept
{
	while (true) {
		std::unique_ptr<px4::PipeServer> pipe(new px4::PipeServer());

		if (!pipe->Accept(pipe_name_, pipe_config_, ready_event, quit_event_))
			break;

		ready_event = NULL;

		try {
			std::lock_guard<std::mutex> lock(mtx_);

			auto &p = conns_.emplace_back(std::unique_ptr<Connection>());
			p.reset(CreateConnection(pipe));
			if (!p->Start())
				break;
		} catch (...) {
			break;
		}
	}

	return;
}

ServerBase::Connection::Connection(ServerBase &parent, std::unique_ptr<px4::PipeServer> &pipe) noexcept
	: parent_(parent),
	conn_(std::move(pipe)),
	config_(parent_.pipe_config_),
	receiver_manager_(parent_.receiver_manager_),
	quit_event_(parent.quit_event_),
	th_()
{

}

ServerBase::Connection::~Connection()
{
	Stop();
	conn_.reset();

	parent_.RemoveConnection(this, false);
}

bool ServerBase::Connection::Start() noexcept
{
	if (th_) {
		error_.assign(EINVAL, std::generic_category());
		return false;
	}

	try {
		th_.reset(new std::thread(&px4::ServerBase::Connection::Worker, this));
	} catch (const std::system_error &e) {
		error_.assign(static_cast<int>(e.code().value()), e.code().category());
		return false;
	}

	return true;
}

bool ServerBase::Connection::Stop() noexcept
{
	if (!th_) {
		error_.assign(EINVAL, std::generic_category());
		return false;
	}

	try {
		if (std::this_thread::get_id() == th_->get_id())
			th_->detach();
		else
			th_->join();
	} catch (...) {}

	th_.reset();

	return true;
}

} // namespace px4
