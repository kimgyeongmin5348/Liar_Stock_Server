#pragma once

#include <cstddef>
#include <cstdint>

#define MAX_PACKET_SIZE 1024
#define SERVER_PORT 3000
#define NUM_WORKER_THREADS 4
#define MAX_USER 5000
#define BUF_SIZE 1024
#define MAX_BUFFER 8192

constexpr std::size_t INITIAL_ACCEPT_COUNT = 16;
constexpr std::size_t RECV_BUFFER_SIZE = MAX_BUFFER;
constexpr std::size_t MAX_ID_LENGTH = 20;

enum class PacketType : std::uint8_t
{
	CS_PING = 1,
	SC_PONG = 2,
};

#pragma pack(push, 1)
struct PacketHeader
{
	std::uint8_t size;
	std::uint8_t type;
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 2);
