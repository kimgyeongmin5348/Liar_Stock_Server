#include "workerthread.h"

atomic_uint g_worker_count = 0;

BOOL WINAPI ConsoleHandler(DWORD signal)
{
	if (signal != CTRL_C_EVENT && signal != CTRL_CLOSE_EVENT && signal != CTRL_BREAK_EVENT) {
		return FALSE;
	}
	if (!g_running.exchange(false)) return TRUE;

	if (g_listen_socket != INVALID_SOCKET) {
		closesocket(g_listen_socket);
		g_listen_socket = INVALID_SOCKET;
	}
	if (g_hIOCP != nullptr) {
		for (unsigned i = 0; i < g_worker_count.load(); ++i) {
			PostQueuedCompletionStatus(g_hIOCP, 0, 0, nullptr);
		}
	}
	return TRUE;
}

int main()
{
	WSADATA wsa_data{};
	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
		cerr << "[ERROR] WSAStartup failed\n";
		return 1;
	}

	int exit_code = 1;
	g_listen_socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
		WSA_FLAG_OVERLAPPED);
	if (g_listen_socket == INVALID_SOCKET) {
		print_error_message("WSASocket(listen)", WSAGetLastError());
		WSACleanup();
		return exit_code;
	}

	BOOL reuse_address = TRUE;
	setsockopt(g_listen_socket, SOL_SOCKET, SO_REUSEADDR,
		reinterpret_cast<const char*>(&reuse_address), sizeof(reuse_address));

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(SERVER_PORT);
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(g_listen_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
		print_error_message("bind", WSAGetLastError());
		closesocket(g_listen_socket);
		WSACleanup();
		return exit_code;
	}
	if (listen(g_listen_socket, SOMAXCONN) == SOCKET_ERROR) {
		print_error_message("listen", WSAGetLastError());
		closesocket(g_listen_socket);
		WSACleanup();
		return exit_code;
	}

	g_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
	if (g_hIOCP == nullptr || CreateIoCompletionPort(
		reinterpret_cast<HANDLE>(g_listen_socket), g_hIOCP, 0, 0) == nullptr) {
		print_error_message("CreateIoCompletionPort(listen)", GetLastError());
		closesocket(g_listen_socket);
		if (g_hIOCP != nullptr) CloseHandle(g_hIOCP);
		WSACleanup();
		return exit_code;
	}

	const unsigned hardware_threads = thread::hardware_concurrency();
	const unsigned worker_count = (max)(1u, (min)(8u, hardware_threads));
	vector<thread> workers;
	workers.reserve(worker_count);
	g_worker_count.store(worker_count);
	SetConsoleCtrlHandler(ConsoleHandler, TRUE);
	for (unsigned i = 0; i < worker_count; ++i) workers.emplace_back(WorkerThread);

	size_t posted_accepts = 0;
	for (size_t i = 0; i < INITIAL_ACCEPT_COUNT; ++i) {
		if (do_accept(g_listen_socket)) ++posted_accepts;
	}
	if (posted_accepts == 0) {
		g_running.store(false);
		for (unsigned i = 0; i < worker_count; ++i) {
			PostQueuedCompletionStatus(g_hIOCP, 0, 0, nullptr);
		}
	}
	else {
		cout << "IOCP server listening on port " << SERVER_PORT
			<< " (workers=" << worker_count << ", accepts=" << posted_accepts
			<< "). Press Ctrl+C to stop.\n";
		exit_code = 0;
	}

	for (auto& worker : workers) worker.join();
	g_worker_count.store(0);
	SetConsoleCtrlHandler(ConsoleHandler, FALSE);

	vector<shared_ptr<SESSION>> sessions;
	{
		lock_guard lock(g_sessions_mutex);
		for (auto& [id, session] : g_sessions) sessions.push_back(move(session));
		g_sessions.clear();
	}
	for (const auto& session : sessions) session->close();

	if (g_listen_socket != INVALID_SOCKET) closesocket(g_listen_socket);
	CloseHandle(g_hIOCP);
	WSACleanup();
	return exit_code;
}
