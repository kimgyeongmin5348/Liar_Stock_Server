# Liar_Stock_Server

> **Windows IOCP (I/O Completion Port) 기반의 고성능 멀티스레드 네트워크 게임 서버 프레임워크**

---

## 📌 1. 아키텍처 개요 (Architecture Overview)

- **I/O 모델**: Windows IOCP (I/O Completion Port) 비동기 통신 모델
- **동시성 모델**: CPU 논리 코어 수 기반의 멀티 워커 스레드 (`GetQueuedCompletionStatus`)
- **접속 수락**: `AcceptEx` API를 통한 사전 비동기 연결 수락 및 풀링 (`INITIAL_ACCEPT_COUNT: 16`)
- **패킷 규격**: 가변 길이 패킷 프레이밍 (`PacketHeader`: 2 Bytes [Size(1B) + Type(1B)])
- **개발 환경**: C++20 / C++ Latest, MSVC x64, Windows Sockets 2 (Winsock)

---

## 💡 2. 핵심 트러블슈팅 및 기술 면접 Q&A (Interview Cheat Sheet)

기술 면접 및 코드 리뷰 시 자주 묻는 네트워크/동시성 핵심 질문과 본 프로젝트에서의 해결 방안입니다.

### Q1. 멀티스레드 환경에서 `WSASend`를 직접 호출할 때의 문제점과 해결 방식은?
* **원인**:
  - 다중 워커 스레드 환경 또는 브로드캐스트(`BroadcastToAll`) 상황에서 한 세션에 대해 여러 스레드가 동시에 `WSASend`를 호출할 수 있습니다.
  - TCP는 바이트 스트림 프로토콜이므로, 이전 `WSASend`의 Overlapped I/O가 완료되기 전에 새로운 `WSASend`가 실행되면 **패킷 데이터 바이트가 중간에 뒤섞이거나(Interleaving) 전송 순서가 역전**되어 클라이언트에서 패킷 파싱 크래시가 발생합니다.
* **해결 방안 (Send Queue 직렬화)**:
  - 세션마다 **스레드 안전한 송신 큐(`_send_queue`)**와 **전송 진행 상태 플래그(`_is_sending`)**를 도입했습니다.
  - `do_send()` 호출 시 패킷 데이터를 큐에 인큐하고, 현재 전송 중인 I/O가 없을 때만 첫 번째 패킷에 대해 `WSASend`를 발행합니다.
  - 전송 완료 통지(`IO_OP::SEND`)가 IOCP를 통해 도착하면, 큐의 완료된 패킷을 제거하고 대기 중인 다음 패킷을 연속해서 송신합니다.
  - **결과**: 수천 개의 비동기 송신 요청이 몰려도 소켓당 오직 1개의 `WSASend`만 활성화 상태를 유지하여 **패킷 전송 순서와 무결성을 100% 보장**합니다.

---

### Q2. 부분 전송(Partial Send) 상황은 어떻게 대응했는가?
* **원인**:
  - OS 송신 소켓 버퍼가 가득 차면 `WSASend`가 요청한 전체 바이트 중 일부만 전송하고 완료될 수 있습니다.
  - 이때 단순하게 다음 패킷으로 넘어가면 잘려나간 데이터가 유실됩니다.
* **해결 방안**:
  - `EXP_OVER` 내부에 `_send_offset`과 `_send_size`를 유지합니다.
  - `on_send_complete()`에서 전송된 바이트(`transferred`)를 누적하여, `_send_offset < _send_size`인 경우 남은 잔여 바이트 구간으로 `WSABUF` 포인터와 길이를 조정한 뒤 재전송(`WSASend`)을 발행합니다.
  - 전체 바이트가 완벽히 전송되었을 때만 `_send_queue.pop()`을 수행하고 다음 패킷을 처리합니다.

---

### Q3. 비동기 I/O 도중 세션 연결이 끊어질 때 메모리 안전성은 어떻게 보장했는가?
* **원인**:
  - 클라이언트가 비정상 종료되거나 강제 접속 해제 시, 이미 커널에 걸려 있는 비동기 I/O(Overlapped I/O)가 완료되기 전에 `SESSION` 객체가 소멸되면 댕글링 포인터(Dangling Pointer) 및 Use-After-Free 크래시가 발생합니다.
* **해결 방안**:
  - `SESSION` 클래스가 `std::enable_shared_from_this<SESSION>`을 상속받도록 설계했습니다.
  - 비동기 I/O 컨텍스트인 `EXP_OVER`가 세션의 `std::shared_ptr<SESSION>`을 보관함으로써, 비동기 작업이 완료되어 완료 포트에 도착할 때까지 세션의 참조 카운트가 1 이상 유지되도록 생명주기를 완벽히 통제했습니다.

---

### Q4. `AcceptEx` 사용 시 DoS(Slowloris / SYN Flood) 공격 방어 고려사항은?
* **원인**:
  - `AcceptEx`의 4번째 매개변수(`dwReceiveDataLength`)를 0보다 크게 설정하면, 클라이언트가 TCP 연결 후 첫 데이터를 보낼 때까지 수락 완료가 발생하지 않아 소켓 및 버퍼 자원이 고갈될 위험이 있습니다.
* **해결 방안**:
  - 4번째 인자를 `0`으로 지정하여 데이터 수신을 기다리지 않고 **3-Way Handshake 완료 즉시 세션을 수락**하도록 구현했습니다.
  - 수락 직후 `SO_UPDATE_ACCEPT_CONTEXT` 옵션을 적용하여 리슨 소켓의 속성을 클라이언트 소켓에 정상 상속하고 즉시 IOCP에 등록합니다.

---

### Q5. TCP 스트림의 패킷 조립(Framing)은 어떻게 처리했는가?
* **원인**:
  - TCP는 패킷 경계를 구분하지 않는 스트림 프로토콜이므로 네트워크 상황에 따라 **패킷 쪼개짐(Fragmentation)**이나 **패킷 뭉침(Coalescing)**이 발생합니다.
* **해결 방안**:
  - `RECV` 완료 핸들러에서 수신 누적 바이트(`_buffered + transferred`)를 기반으로 헤더(`PacketHeader`)의 `size` 필드를 파싱합니다.
  - 완전한 패킷 크기만큼 수신된 경우에만 패킷 처리기(`process_packet`)로 넘겨주고, 루프를 돌며 누적 버퍼 내의 복수 패킷을 모두 처리합니다.
  - 미완성된 잔여 바이트는 `std::memmove`를 통해 버퍼의 선두로 이동시키고 다음 `WSARecv`에서 이어받도록 처리했습니다.

---

## 🚀 3. 프로젝트 구조 (Directory Structure)

```plaintext
Liar_Stock_Server/
├── common.h         # 공용 시스템 헤더, 네트워크 라이브러리 링크 및 환경 매크로
├── protocol.h       # 서버 매크로 상수, 패킷 구조체 및 패킷 타입 정의
├── workerthread.h   # EXP_OVER, SESSION 클래스 선언 및 IOCP 워커 스레드 시그니처
├── workerthread.cpp # 세션 수명주기, Send Queue 직렬화, AcceptEx 및 워커 스레드 구현
└── main.cpp         # 소켓 초기화, IOCP 생성, 워커 스레드 풀 스케일링 및 Graceful Shutdown
```

---

## 🛠️ 4. 빌드 및 실행 방법

### 요구 사양
- OS: Windows 10 / 11 / Windows Server
- 컴파일러: MSVC v143 이상 (Visual Studio 2022 / 2026 Preview)
- 플랫폼: x64

### 빌드 (MSBuild)
```powershell
# Release 빌드
msbuild Liar_Stock_Server\Liar_Stock_Server.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild

# Debug 빌드
msbuild Liar_Stock_Server\Liar_Stock_Server.vcxproj /p:Configuration=Debug /p:Platform=x64 /t:Rebuild
```