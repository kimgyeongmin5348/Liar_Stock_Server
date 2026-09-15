#include "workerthread.h"
#include "protocol.h"

class SESSION;

HANDLE g_hIOCP;
std::unordered_map<long long, SESSION*> g_session;
std::mutex g_session_mutex;
std::shared_mutex g_session_lifetime_mutex;
std::mutex g_log_mutex;
SOCKET g_listen_socket = INVALID_SOCKET;
//std::atomic<long long> g_session_id_counter = 0;
long long g_session_id_counter = 0;


// SESSION 구현
SESSION::SESSION(long long session_id, SOCKET s) : _id(session_id), _c_socket(s), _recv_over(IO_RECV)
{
	// 소켓 옵션 추가 (Keep-Alive 설정)
	int opt = 1;
	setsockopt(_c_socket, SOL_SOCKET, SO_KEEPALIVE, (char*)&opt, sizeof(opt));

	// Nagle 알고리즘 비활성화 (실시간 통신 필수)
	setsockopt(_c_socket, IPPROTO_TCP, TCP_NODELAY, (char*)&opt, sizeof(opt));

	{
		std::lock_guard<std::mutex> lock(g_session_mutex);
		g_session[_id] = this;
		std::cout << "[서버] 세션 추가 완료: ID=" << _id << ", 현재 접속자 수: " << g_session.size() << "\n";
	}
	_remained = 0;
	do_recv();
}

void SESSION::do_recv() {

	if (_c_socket == INVALID_SOCKET) return;

	DWORD recv_flag = 0;
	ZeroMemory(&_recv_over._over, sizeof(_recv_over._over));
	_recv_over._wsabuf[0].buf = reinterpret_cast<CHAR*>(_recv_over._buffer + _remained);
	_recv_over._wsabuf[0].len = sizeof(_recv_over._buffer) - _remained;

	auto ret = WSARecv(_c_socket, _recv_over._wsabuf, 1, NULL, &recv_flag, &_recv_over._over, NULL);
	if (0 != ret) {
		auto err_no = WSAGetLastError();
		if (WSA_IO_PENDING != err_no) {
			std::cout << "[오류] " << _id << "번 클라이언트 연결 종료. 코드: " << err_no << "\n";
			return;
		}
	}
	/*std::cout << "[서버] " << _id << "번 소켓 수신 대기 시작\n";*/
}

void SESSION::do_send(void* buff) {
	SOCKET sock = _c_socket;
	if (sock == INVALID_SOCKET || _pendingDelete || buff == nullptr) return;

	EXP_OVER* over = new EXP_OVER(IO_SEND);
	const unsigned char packet_size = reinterpret_cast<unsigned char*>(buff)[0];
	if (packet_size < 2) {
		delete over;
		return;
	}
	memcpy(over->_buffer, buff, packet_size);
	over->_wsabuf[0].len = packet_size;

	int ret = WSASend(sock, over->_wsabuf, 1, NULL, 0, &over->_over, NULL);
	if (ret == SOCKET_ERROR) {
		const int error = WSAGetLastError();
		if (error != WSA_IO_PENDING) {
			// These are expected when a peer is closing during a mass disconnect.
			if (error != WSAECONNABORTED && error != WSAECONNRESET && error != WSA_OPERATION_ABORTED) {
				std::lock_guard<std::mutex> logLock(g_log_mutex);
				std::cerr << "[ERROR] do_send failed ID=" << _id << " error=" << error << '\n';
			}
			delete over;
		}
	}
}

void SESSION::process_packet(unsigned char* p)
{
	const unsigned char packet_type = p[1];
	switch (packet_type) {
	case 1:
	{

	}

	default:
		std::cout << "[경고] 잘못된 패킷 타입: " << (int)packet_type << "\n";
		return;
	}

}

void BroadcastToAll(void* pkt, long long exclude_id = -1) {
	unsigned char packet_size = reinterpret_cast<unsigned char*>(pkt)[0];
	std::vector<SESSION*> sessions;
	{
		std::lock_guard<std::mutex> lock(g_session_mutex);
		for (auto& pair : g_session) {
			if (pair.second->_c_socket != INVALID_SOCKET && pair.first != exclude_id) {
				sessions.push_back(pair.second);
			}
		}
	}
	for (auto* session : sessions) {
		session->do_send(pkt);  // do_send 호출로 통일
	}
}

void CloseSession(long long id)
{
	SESSION* pSession = nullptr;
	std::size_t remainingSessions = 0;
	{
		std::lock_guard<std::mutex> lock(g_session_mutex);
		auto it = g_session.find(id);
		if (it == g_session.end()) return;
		pSession = it->second;
		pSession->_pendingDelete = true;
		g_session.erase(it);
		remainingSessions = g_session.size();
	}

	if (!pSession) return;
	SOCKET s = pSession->_c_socket;
	pSession->_c_socket = INVALID_SOCKET;
	if (s != INVALID_SOCKET) {
		shutdown(s, SD_BOTH);
		closesocket(s);
	}

	char savedPlayerID[MAX_ID_LENGTH] = {};
	strncpy_s(savedPlayerID, sizeof(savedPlayerID), pSession->_playerID, _TRUNCATE);
	const long long savedID = pSession->_id;

	// Stress clients disconnect as one batch. Broadcasting N leave packets to the
	// other N-1 sockets creates an unnecessary O(N^2) teardown storm.
	const bool isStressClient = strncmp(savedPlayerID, "Stress_", 7) == 0;
	if (!isStressClient) {
		sc_packet_leave leavePkt{};
		leavePkt.size = sizeof(leavePkt);
		leavePkt.type = SC_P_LEAVE;
		leavePkt.id = savedID;
		strncpy_s(leavePkt.playerID, savedPlayerID, MAX_ID_LENGTH - 1);
		BroadcastToAll(&leavePkt, savedID);
	}

	if (!isStressClient || remainingSessions == 0 || (remainingSessions % 100) == 0) {
		std::lock_guard<std::mutex> logLock(g_log_mutex);
		if (isStressClient) {
			std::cout << "[STRESS DISCONNECT] remaining=" << remainingSessions << '\n';
		}
		else {
			std::cout << "[DISCONNECT] playerID=" << savedPlayerID << " sessionID=" << savedID
				<< " remaining=" << remainingSessions << '\n';
		}
	}
}

void print_error_message(int s_err)
{
	WCHAR* lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, s_err,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	std::wcout << lpMsgBuf << std::endl;
	//while (true); // 디버깅 용
	LocalFree(lpMsgBuf);
}

void do_accept(SOCKET s_socket) {
	EXP_OVER* accept_over = new EXP_OVER(IO_ACCEPT);
	SOCKET c_socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, 0, 0, WSA_FLAG_OVERLAPPED);

	// 소켓 옵션 설정 (Nagle 알고리즘 비활성화)
	int opt = 1;
	setsockopt(c_socket, IPPROTO_TCP, TCP_NODELAY, (char*)&opt, sizeof(opt));

	accept_over->_accept_socket = c_socket;

	// AcceptEx 호출
	if (!AcceptEx(s_socket, c_socket, accept_over->_buffer, 0,
		sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
		NULL, &accept_over->_over))
	{
		int err = WSAGetLastError();
		if (err != ERROR_IO_PENDING) {
			print_error_message(err);
			delete accept_over;
			closesocket(c_socket);
		}
	}
}

void WorkerThread() {
	while (true) {
		DWORD io_size;
		WSAOVERLAPPED* o;
		ULONG_PTR key;
		BOOL ret = GetQueuedCompletionStatus(g_hIOCP, &io_size, &key, &o, INFINITE);

		if (o == nullptr) {
			cout << "[오류] GQCS o=NULL, key=" << key << "\n";
			continue;
		}

		EXP_OVER* eo = reinterpret_cast<EXP_OVER*>(o);

		if (FALSE == ret || (0 == io_size && (eo->_io_op == IO_RECV || eo->_io_op == IO_SEND))) {
			if (eo->_io_op == IO_RECV) {
				std::unique_lock<std::shared_mutex> lifetimeLock(g_session_lifetime_mutex);

				EXP_OVER* recvOver = eo;
				SESSION* pSession = reinterpret_cast<SESSION*>(
					reinterpret_cast<char*>(recvOver)
					- offsetof(SESSION, _recv_over)
					);

				long long disconnected_id = static_cast<long long>(key);

				if (!pSession->_pendingDelete)
				{
					// 아직 CloseSession 안 불린 경우 (클라이언트 강제종료)
					CloseSession(disconnected_id);
				}
				delete pSession;
			}
			else {
				delete eo;
			}
			continue;
		}

		switch (eo->_io_op)
		{
		case IO_ACCEPT:
		{

			long long new_id = ++g_session_id_counter;
			SOCKET client_socket = eo->_accept_socket;

			// 1. 클라이언트 주소 정보 추출
			SOCKADDR_IN* client_addr = nullptr;
			SOCKADDR_IN* local_addr = nullptr;
			int remote_addr_len = sizeof(SOCKADDR_IN);
			int local_addr_len = sizeof(SOCKADDR_IN);

			GetAcceptExSockaddrs(
				eo->_buffer, 0,
				sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
				(SOCKADDR**)&local_addr, &local_addr_len,
				(SOCKADDR**)&client_addr, &remote_addr_len
			);

			// 2. IP 주소 문자열 변환
			char ip_str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &(client_addr->sin_addr), ip_str, INET_ADDRSTRLEN);
			std::cout << "[서버] 새로운 클라이언트 접속: IP=" << ip_str
				<< ", 포트=" << ntohs(client_addr->sin_port)
				<< ", 할당 ID=" << new_id << "\n";

			// 3. IOCP에 소켓 등록
			CreateIoCompletionPort(reinterpret_cast<HANDLE>(client_socket), g_hIOCP, new_id, 0);

			// 4. 세션 생성
			new SESSION(new_id, client_socket);

			// 5. 다음 Accept 요청
			do_accept(g_listen_socket);

			// 6. 현재 OVERLAPPED 메모리 해제
			delete eo;
			break;

		}

		case IO_SEND:
		{
			delete eo;
			break;
		}

		case IO_RECV:
		{
			std::shared_lock<std::shared_mutex> lifetimeLock(g_session_lifetime_mutex);
			// 1. 뮤텍스 락으로 세션 검색 (스레드 세이프)
			SESSION* pUser = nullptr;
			{
				std::lock_guard<std::mutex> lock(g_session_mutex);
				auto it = g_session.find(key);
				if (it == g_session.end()) {
					// 세션이 이미 제거된 경우
					//delete eo;  // EXP_OVER 객체 정리
					continue;
				}
				pUser = it->second;  // 포인터 추출
			}

			/*if (FALSE == ret || 0 == io_size) {
				cout << "[접속종료-내부] sessionID=" << key << "\n";
				delete eo;
				continue;
			}*/

			// 2. 세션 작업 (락이 해제된 상태에서 진행)
			SESSION& user = *pUser;  // 역참조

			unsigned char* p = eo->_buffer;
			unsigned char* bufferEnd = eo->_buffer + io_size + user._remained;

			while (p < bufferEnd) {
				const std::size_t available = static_cast<std::size_t>(bufferEnd - p);
				if (available < 2) break; // A partial header is normal on a TCP stream.

				const unsigned char packet_size = p[0];
				if (packet_size < 2 || packet_size > MAX_PACKET_SIZE) {
					std::cerr << "[ERROR] Invalid packet size: " << static_cast<int>(packet_size) << "\n";
					p = bufferEnd; // Discard a malformed stream instead of retaining corrupt bytes.
					break;
				}

				if (available < packet_size) {
					// The packet is valid but incomplete. Preserve it for the next WSARecv.
					break;
				}

				user.process_packet(p);
				p += packet_size;
			}

			const std::size_t remained = static_cast<std::size_t>(bufferEnd - p);
			if (remained > 0) {
				memmove(eo->_buffer, p, remained);
			}
			user._remained = static_cast<unsigned char>(remained);
			//delete eo;
			pUser->do_recv();
			break;
		}
		}
	}
}
