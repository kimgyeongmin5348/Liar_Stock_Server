#pragma once

#include "common.h"
#include "protocol.h"

class SESSION;

extern HANDLE g_hIOCP;
extern std::unordered_map<std::uint64_t, std::shared_ptr<SESSION>> g_sessions;
extern std::mutex g_sessions_mutex;
extern std::mutex g_log_mutex;
extern SOCKET g_listen_socket;
extern std::atomic_bool g_running;

enum class IO_OP { RECV, SEND, ACCEPT };

class EXP_OVER
{
public:
	explicit EXP_OVER(IO_OP op, std::shared_ptr<SESSION> owner = {});

	WSAOVERLAPPED _over{};
	IO_OP _io_op;
	SOCKET _accept_socket = INVALID_SOCKET;
	std::shared_ptr<SESSION> _owner;
	std::array<unsigned char, RECV_BUFFER_SIZE> _buffer{};
	WSABUF _wsabuf{};
	std::size_t _buffered = 0;
	std::size_t _send_offset = 0;
	std::size_t _send_size = 0;
};

class SESSION : public std::enable_shared_from_this<SESSION>
{
public:
	SESSION(std::uint64_t session_id, SOCKET socket);
	~SESSION();

	SESSION(const SESSION&) = delete;
	SESSION& operator=(const SESSION&) = delete;

	void start();
	void do_recv();
	void do_send(const void* packet, std::size_t packet_size);
	bool on_send_complete(std::size_t transferred, EXP_OVER* context);
	void process_packet(const unsigned char* packet, std::size_t packet_size);
	void close();

	[[nodiscard]] std::uint64_t id() const noexcept { return _id; }
	[[nodiscard]] bool is_closing() const noexcept { return _closing.load(); }

private:
	void send_next_locked();

	friend void WorkerThread();

	std::mutex _socket_mutex;
	SOCKET _socket;
	const std::uint64_t _id;
	std::atomic_bool _closing{ false };

	std::mutex _send_mutex;
	std::queue<std::vector<unsigned char>> _send_queue;
	bool _is_sending = false;
};

void CloseSession(std::uint64_t id);
void BroadcastToAll(const void* packet, std::size_t packet_size, std::uint64_t exclude_id = 0);
void print_error_message(const char* operation, int error);
bool do_accept(SOCKET listen_socket);
void WorkerThread();
