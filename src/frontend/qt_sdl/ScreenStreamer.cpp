#include "ScreenStreamer.h"

#include <cstring>
#include <algorithm>
#include <thread>
#include <unistd.h>

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t frameId;
    uint16_t chunkIndex;
    uint16_t totalChunks;
};
#pragma pack(pop)

static constexpr int HEADER_SIZE  = sizeof(PacketHeader);
static constexpr int CHUNK_SIZE   = 60000;
static constexpr int MAX_PACKET   = HEADER_SIZE + CHUNK_SIZE;
sockaddr_in clientAddr;
socklen_t clientLen;

ScreenStreamer::ScreenStreamer(uint16_t port) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#else
    sock = socket(AF_INET, SOCK_DGRAM, 0);
#endif

    int bufSize = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF,
                reinterpret_cast<const char*>(&bufSize),
                sizeof(bufSize));

    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
#endif
    std::thread(&ScreenStreamer::listenForBroadcast, this).detach();
}

ScreenStreamer::~ScreenStreamer() {
#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif
}

void ScreenStreamer::sendFrame(const void* bgraData, int width, int height) {
    const uint8_t* data = static_cast<const uint8_t*>(bgraData);

    int totalBytes  = width * height * 4;
    int totalChunks  = (totalBytes + CHUNK_SIZE - 1) / CHUNK_SIZE;

    uint8_t packet[MAX_PACKET];
    auto* hdr = reinterpret_cast<PacketHeader*>(packet);

    for (int i = 0; i < totalChunks; i++) {
        int offset = i * CHUNK_SIZE;
        int size   = std::min(CHUNK_SIZE, totalBytes - offset);

        hdr->frameId     = frameId;
        hdr->chunkIndex  = static_cast<uint16_t>(i);
        hdr->totalChunks = static_cast<uint16_t>(totalChunks);

        std::memcpy(packet + HEADER_SIZE, data + offset, size);

        sendto(sock,
               reinterpret_cast<const char*>(packet),
               HEADER_SIZE + size,
               0,
               reinterpret_cast<sockaddr*>(&clientAddr),
               sizeof(&clientAddr));
    }

    frameId++;
}

void ScreenStreamer::listenForBroadcast() {
    int recvSock = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5000);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(recvSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("bind failed");
        return;
    }

    int sendSock = socket(AF_INET, SOCK_DGRAM, 0);

    char buffer[256];
    sockaddr_in client{};
    socklen_t len = sizeof(client);

    while (true) {
        int r = recvfrom(recvSock, buffer, sizeof(buffer) - 1, 0,
                         (sockaddr*)&client, &len);

        if (r > 0) {
            buffer[r] = '\0';

            std::string msg(buffer);

            if (msg == "DISCOVER_SERVER") {
                clientAddr = client;
                clientLen = len;
                const char* reply = "SERVER_HERE";

                sendto(sendSock, reply, strlen(reply), 0,
                       (sockaddr*)&client, len);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}