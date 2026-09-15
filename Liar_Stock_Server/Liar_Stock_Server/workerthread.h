#pragma once
#include "common.h"
#include "protocol.h"

class SESSION;

// 전역 변수 선언
extern HANDLE g_hIOCP;
extern std::atomic<long long> g_client_counter;
extern std::unordered_map<long long, SESSION*> g_session;
extern std::mutex g_session_mutex;
extern std::shared_mutex g_session_lifetime_mutex;
extern std::mutex g_log_mutex;
extern SOCKET g_listen_socket;

enum IO_OP { IO_RECV, IO_SEND, IO_ACCEPT };


class EXP_OVER
{
public:
	EXP_OVER(IO_OP op) : _io_op(op) {
		ZeroMemory(&_over, sizeof(_over));
		_wsabuf[0].buf = reinterpret_cast<CHAR*>(_buffer);
		_wsabuf[0].len = sizeof(_buffer);
	}

	WSAOVERLAPPED		_over;
	IO_OP				_io_op;
	SOCKET				_accept_socket;
	unsigned char		_buffer[256];
	WSABUF				_wsabuf[1];

};


class SESSION {
public:

	SOCKET				_c_socket;
	long long			_id;
	long long			_nickname;
	char				_playerID[MAX_ID_LENGTH];

	EXP_OVER			_recv_over{ IO_RECV };
	unsigned char		_remained;

	bool				_isGameReady = false;
	bool				_hasLoggedPreReadyMove = false;
	bool				_hasAcceptedFirstMove = false;
	bool				_pendingDelete = false;

public:
	SESSION() = delete;

	SESSION(long long session_id, SOCKET s);

	~SESSION() = default;

	//~SESSION() {
	//	if (_c_socket != INVALID_SOCKET) {
	//		closesocket(_c_socket);
	//		_c_socket = INVALID_SOCKET;
	//	}
	//}

	void do_recv();
	void do_send(void* buff);
	void send_player_info_packet();
	void process_packet(unsigned char* p);

};

void CloseSession(long long id);
void BroadcastToAll(void* pkt, long long exclude_id);
void print_error_message(int s_err);
void do_accept(SOCKET s_socket);
void CloseSession(long long id);
void WorkerThread();
