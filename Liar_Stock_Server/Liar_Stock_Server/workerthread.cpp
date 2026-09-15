#include "workerthread.h"

HANDLE g_hIOCP = nullptr;
std::unordered_map<std::uint64_t, std::shared_ptr<SESSION>> g_sessions;
std::mutex g_sessions_mutex;
std::mutex g_log_mutex;
SOCKET g_listen_socket = INVALID_SOCKET;
std::atomic_bool g_running{ true };

namespace
{
	std::atomic_uint64_t g_next_session_id{ 0 };

	bool is_expected_disconnect_error(int error)
	{
		return error == ERROR_NETNAME_DELETED || error == WSAECONNABORTED ||
			error == WSAECONNRESET || error == WSA_OPERATION_ABORTED;
	}
}

EXP_OVER::EXP_OVER(IO_OP op, std::shared_ptr<SESSION> owner)
	: _io_op(op), _owner(std::move(owner))
{
	_wsabuf.buf = reinterpret_cast<char*>(_buffer.data());
	_wsabuf.len = static_cast<ULONG>(_buffer.size());
}

SESSION::SESSION(std::uint64_t session_id, SOCKET socket)
	: _socket(socket), _id(session_id)
{
	BOOL enabled = TRUE;
	if (setsockopt(_socket, SOL_SOCKET, SO_KEEPALIVE,
		reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == SOCKET_ERROR) {
		print_error_message("setsockopt(SO_KEEPALIVE)", WSAGetLastError());
	}
	if (setsockopt(_socket, IPPROTO_TCP, TCP_NODELAY,
		reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == SOCKET_ERROR) {
		print_error_message("setsockopt(TCP_NODELAY)", WSAGetLastError());
	}
}

SESSION::~SESSION()
{
	close();
}

void SESSION::start()
{
	do_recv();
}

void SESSION::do_recv()
{
	if (_closing.load()) return;

	auto* context = new EXP_OVER(IO_OP::RECV, shared_from_this());
	DWORD flags = 0;
	int result;
	{
		std::lock_guard lock(_socket_mutex);
		if (_socket == INVALID_SOCKET || _closing.load()) {
			delete context;
			return;
		}
		result = WSARecv(_socket, &context->_wsabuf, 1, nullptr, &flags,
			&context->_over, nullptr);
	}

	if (result == SOCKET_ERROR) {
		const int error = WSAGetLastError();
		if (error != WSA_IO_PENDING) {
			delete context;
			if (!is_expected_disconnect_error(error)) print_error_message("WSARecv", error);
			CloseSession(_id);
		}
	}
}

void SESSION::do_send(const void* packet, std::size_t packet_size)
{
	if (packet == nullptr || packet_size < sizeof(PacketHeader) ||
		packet_size > MAX_PACKET_SIZE || _closing.load()) return;

	std::lock_guard lock(_send_mutex);
	if (_closing.load()) return;

	const auto* byte_ptr = reinterpret_cast<const unsigned char*>(packet);
	_send_queue.emplace(byte_ptr, byte_ptr + packet_size);

	if (!_is_sending) {
		send_next_locked();
	}
}

void SESSION::send_next_locked()
{
	if (_send_queue.empty() || _closing.load()) {
		_is_sending = false;
		return;
	}

	_is_sending = true;
	const auto& front_packet = _send_queue.front();

	auto* context = new EXP_OVER(IO_OP::SEND, shared_from_this());
	std::memcpy(context->_buffer.data(), front_packet.data(), front_packet.size());
	context->_wsabuf.buf = reinterpret_cast<char*>(context->_buffer.data());
	context->_wsabuf.len = static_cast<ULONG>(front_packet.size());
	context->_send_size = front_packet.size();
	context->_send_offset = 0;

	int result;
	{
		std::lock_guard lock(_socket_mutex);
		if (_socket == INVALID_SOCKET || _closing.load()) {
			delete context;
			_is_sending = false;
			return;
		}
		result = WSASend(_socket, &context->_wsabuf, 1, nullptr, 0,
			&context->_over, nullptr);
	}

	if (result == SOCKET_ERROR) {
		const int error = WSAGetLastError();
		if (error != WSA_IO_PENDING) {
			delete context;
			_is_sending = false;
			if (!is_expected_disconnect_error(error)) print_error_message("WSASend", error);
			CloseSession(_id);
		}
	}
}

bool SESSION::on_send_complete(std::size_t transferred, EXP_OVER* context)
{
	if (transferred == 0 || _closing.load()) {
		CloseSession(_id);
		return false;
	}

	context->_send_offset += transferred;
	if (context->_send_offset < context->_send_size) {
		context->_wsabuf.buf = reinterpret_cast<char*>(
			context->_buffer.data() + context->_send_offset);
		context->_wsabuf.len = static_cast<ULONG>(context->_send_size - context->_send_offset);
		ZeroMemory(&context->_over, sizeof(context->_over));

		int result;
		{
			std::lock_guard lock(_socket_mutex);
			if (_socket == INVALID_SOCKET || _closing.load()) return false;
			result = WSASend(_socket, &context->_wsabuf, 1, nullptr, 0,
				&context->_over, nullptr);
		}
		if (result == 0) return true;
		const int error = WSAGetLastError();
		if (error == WSA_IO_PENDING) return true;
		if (!is_expected_disconnect_error(error)) print_error_message("WSASend(partial)", error);
		CloseSession(_id);
		return false;
	}

	std::lock_guard lock(_send_mutex);
	if (!_send_queue.empty()) {
		_send_queue.pop();
	}
	send_next_locked();
	return false;
}

void SESSION::process_packet(const unsigned char* packet, std::size_t packet_size)
{
	if (packet_size < sizeof(PacketHeader)) return;

	const auto type = static_cast<PacketType>(packet[1]);
	switch (type) {
	case PacketType::CS_PING:
	{
		const PacketHeader pong{
			static_cast<std::uint8_t>(sizeof(PacketHeader)),
			static_cast<std::uint8_t>(PacketType::SC_PONG)
		};
		do_send(&pong, sizeof(pong));
		break;
	}
	default:
		std::lock_guard log_lock(g_log_mutex);
		std::cerr << "[WARN] unknown packet type=" << static_cast<unsigned>(packet[1])
			<< " session=" << _id << '\n';
		break;
	}
}

void SESSION::close()
{
	if (_closing.exchange(true)) return;

	{
		std::lock_guard lock(_send_mutex);
		std::queue<std::vector<unsigned char>> empty;
		std::swap(_send_queue, empty);
		_is_sending = false;
	}

	std::lock_guard lock(_socket_mutex);
	if (_socket != INVALID_SOCKET) {
		shutdown(_socket, SD_BOTH);
		closesocket(_socket);
		_socket = INVALID_SOCKET;
	}
}

void CloseSession(std::uint64_t id)
{
	std::shared_ptr<SESSION> session;
	std::size_t remaining = 0;
	{
		std::lock_guard lock(g_sessions_mutex);
		const auto it = g_sessions.find(id);
		if (it == g_sessions.end()) return;
		session = std::move(it->second);
		g_sessions.erase(it);
		remaining = g_sessions.size();
	}

	session->close();
	std::lock_guard log_lock(g_log_mutex);
	std::cout << "[DISCONNECT] session=" << id << " remaining=" << remaining << '\n';
}

void BroadcastToAll(const void* packet, std::size_t packet_size, std::uint64_t exclude_id)
{
	std::vector<std::shared_ptr<SESSION>> sessions;
	{
		std::lock_guard lock(g_sessions_mutex);
		sessions.reserve(g_sessions.size());
		for (const auto& [id, session] : g_sessions) {
			if (id != exclude_id && !session->is_closing()) sessions.push_back(session);
		}
	}
	for (const auto& session : sessions) session->do_send(packet, packet_size);
}

void print_error_message(const char* operation, int error)
{
	char* message = nullptr;
	FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
		reinterpret_cast<char*>(&message), 0, nullptr);

	std::lock_guard log_lock(g_log_mutex);
	std::cerr << "[ERROR] " << operation << " failed (" << error << ")";
	if (message != nullptr) {
		std::cerr << ": " << message;
		LocalFree(message);
	}
	std::cerr << '\n';
}

bool do_accept(SOCKET listen_socket)
{
	auto* context = new EXP_OVER(IO_OP::ACCEPT);
	context->_accept_socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
		WSA_FLAG_OVERLAPPED);
	if (context->_accept_socket == INVALID_SOCKET) {
		print_error_message("WSASocket(accept)", WSAGetLastError());
		delete context;
		return false;
	}

	constexpr DWORD address_size = sizeof(sockaddr_in) + 16;
	DWORD bytes_received = 0;
	if (!AcceptEx(listen_socket, context->_accept_socket, context->_buffer.data(), 0,
		address_size, address_size, &bytes_received, &context->_over)) {
		const int error = WSAGetLastError();
		if (error != ERROR_IO_PENDING) {
			print_error_message("AcceptEx", error);
			closesocket(context->_accept_socket);
			delete context;
			return false;
		}
	}
	return true;
}

void WorkerThread()
{
	while (true) {
		DWORD transferred = 0;
		ULONG_PTR completion_key = 0;
		OVERLAPPED* overlapped = nullptr;
		const BOOL success = GetQueuedCompletionStatus(g_hIOCP, &transferred,
			&completion_key, &overlapped, INFINITE);

		if (overlapped == nullptr) {
			if (!g_running.load()) break;
			print_error_message("GetQueuedCompletionStatus", GetLastError());
			continue;
		}

		std::unique_ptr<EXP_OVER> context(reinterpret_cast<EXP_OVER*>(overlapped));
		if (!success) {
			const int error = GetLastError();
			if (context->_io_op == IO_OP::ACCEPT) {
				closesocket(context->_accept_socket);
				if (g_running.load()) do_accept(g_listen_socket);
			}
			else if (context->_owner) {
				const auto id = context->_owner->id();
				if (!is_expected_disconnect_error(error)) print_error_message("overlapped I/O", error);
				CloseSession(id);
			}
			continue;
		}

		switch (context->_io_op) {
		case IO_OP::ACCEPT:
		{
			const SOCKET client_socket = context->_accept_socket;
			if (setsockopt(client_socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
				reinterpret_cast<const char*>(&g_listen_socket), sizeof(g_listen_socket)) == SOCKET_ERROR) {
				print_error_message("setsockopt(SO_UPDATE_ACCEPT_CONTEXT)", WSAGetLastError());
				closesocket(client_socket);
				if (g_running.load()) do_accept(g_listen_socket);
				break;
			}

			const std::uint64_t id = ++g_next_session_id;
			if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(client_socket), g_hIOCP,
				static_cast<ULONG_PTR>(id), 0) == nullptr) {
				print_error_message("CreateIoCompletionPort(client)", GetLastError());
				closesocket(client_socket);
				if (g_running.load()) do_accept(g_listen_socket);
				break;
			}

			auto session = std::make_shared<SESSION>(id, client_socket);
			bool accepted = false;
			{
				std::lock_guard lock(g_sessions_mutex);
				if (g_sessions.size() < MAX_USER) {
					g_sessions.emplace(id, session);
					accepted = true;
				}
			}
			if (!accepted) {
				session->close();
				if (g_running.load()) do_accept(g_listen_socket);
				break;
			}
			{
				std::lock_guard log_lock(g_log_mutex);
				std::cout << "[ACCEPT] session=" << id << '\n';
			}
			session->start();
			if (g_running.load()) do_accept(g_listen_socket);
			break;
		}
		case IO_OP::SEND:
		{
			auto session = context->_owner;
			if (!session || transferred == 0) {
				if (session) CloseSession(session->id());
				break;
			}
			if (session->on_send_complete(transferred, context.get())) {
				context.release();
			}
			break;
		}
		case IO_OP::RECV:
		{
			auto session = context->_owner;
			if (!session || transferred == 0) {
				if (session) CloseSession(session->id());
				break;
			}

			const std::size_t total_size = context->_buffered + transferred;
			std::size_t offset = 0;
			bool malformed = false;
			while (offset < total_size) {
				const std::size_t available = total_size - offset;
				if (available < sizeof(PacketHeader)) break;
				const std::size_t packet_size = context->_buffer[offset];
				if (packet_size < sizeof(PacketHeader) || packet_size > MAX_PACKET_SIZE) {
					malformed = true;
					break;
				}
				if (available < packet_size) break;
				session->process_packet(context->_buffer.data() + offset, packet_size);
				offset += packet_size;
			}

			const std::size_t remaining = total_size - offset;
			if (malformed || remaining >= context->_buffer.size()) {
				CloseSession(session->id());
				break;
			}
			if (remaining > 0) {
				std::memmove(context->_buffer.data(), context->_buffer.data() + offset, remaining);
			}
			context->_buffered = remaining;
			context->_wsabuf.buf = reinterpret_cast<char*>(context->_buffer.data() + remaining);
			context->_wsabuf.len = static_cast<ULONG>(context->_buffer.size() - remaining);
			ZeroMemory(&context->_over, sizeof(context->_over));

			int result;
			DWORD flags = 0;
			{
				std::lock_guard lock(session->_socket_mutex);
				if (session->_socket == INVALID_SOCKET || session->_closing.load()) break;
				result = WSARecv(session->_socket, &context->_wsabuf, 1, nullptr, &flags,
					&context->_over, nullptr);
			}
			if (result == 0) {
				context.release();
			}
			else {
				const int error = WSAGetLastError();
				if (error == WSA_IO_PENDING) context.release();
				else {
					if (!is_expected_disconnect_error(error)) print_error_message("WSARecv", error);
					CloseSession(session->id());
				}
			}
			break;
		}
		}
	}
}
