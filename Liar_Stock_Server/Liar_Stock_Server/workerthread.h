#pragma once

#include "common.h"
#include "protocol.h"

class SESSION;

extern HANDLE g_hIOCP;
extern unordered_map<uint64_t, shared_ptr<SESSION>> g_sessions;
extern mutex g_sessions_mutex;
extern mutex g_log_mutex;
extern SOCKET g_listen_socket;
extern atomic_bool g_running;

enum class IO_OP { RECV, SEND, ACCEPT };

class EXP_OVER
{
public:
	explicit EXP_OVER(IO_OP op, shared_ptr<SESSION> owner = {});

	WSAOVERLAPPED _over{};
	IO_OP _io_op;
	SOCKET _accept_socket = INVALID_SOCKET;
	shared_ptr<SESSION> _owner;
	array<unsigned char, RECV_BUFFER_SIZE> _buffer{};
	WSABUF _wsabuf{};
	size_t _buffered = 0;
	size_t _send_offset = 0;
	size_t _send_size = 0;
};

class SESSION : public enable_shared_from_this<SESSION>
{
public:
	SESSION(uint64_t session_id, SOCKET socket);
	~SESSION();

	SESSION(const SESSION&) = delete;
	SESSION& operator=(const SESSION&) = delete;

	void start();
	void do_recv();
	void do_send(const void* packet, size_t packet_size);
	bool on_send_complete(size_t transferred, EXP_OVER* context);
	void process_packet(const unsigned char* packet, size_t packet_size);
	void close();

	[[nodiscard]] uint64_t id() const noexcept { return _id; }
	[[nodiscard]] bool is_closing() const noexcept { return _closing.load(); }

private:
	void send_next_locked();

	friend void WorkerThread();

	mutex _socket_mutex;
	SOCKET _socket;
	const uint64_t _id;
	atomic_bool _closing{ false };

	mutex _send_mutex;
	queue<vector<unsigned char>> _send_queue;
	bool _is_sending = false;
};

void CloseSession(uint64_t id);
void BroadcastToAll(const void* packet, size_t packet_size, uint64_t exclude_id = 0);
void print_error_message(const char* operation, int error);
bool do_accept(SOCKET listen_socket);
void WorkerThread();
