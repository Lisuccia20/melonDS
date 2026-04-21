#include "ScreenStreamer.h"

#include <cstring>
#include <algorithm>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <string>
#include <iostream>

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t frameId;
    uint16_t chunkIndex;
    uint16_t totalChunks;
};
#pragma pack(pop)

static constexpr int HEADER_SIZE = sizeof(PacketHeader);
static constexpr int CHUNK_SIZE  = 60000;
static constexpr int MAX_PACKET  = HEADER_SIZE + CHUNK_SIZE;

// stato client
static sockaddr_in clientAddr{};
static socklen_t clientLen = sizeof(clientAddr);
static std::atomic<bool> clientReady = false;

ScreenStreamer::ScreenStreamer(uint16_t port) {
    sock = socket(AF_INET, SOCK_DGRAM, 0);

    int bufSize = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&bufSize),
               sizeof(bufSize));

    std::thread(&ScreenStreamer::listenLoop, this).detach();
}

ScreenStreamer::~ScreenStreamer() {
    close(sock);
}

void ScreenStreamer::sendFrame(const void* bgraData, int width, int height) {
    if (!clientReady) return;

    const uint8_t* data = static_cast<const uint8_t*>(bgraData);

    int totalBytes = width * height * 4;
    int totalChunks = (totalBytes + CHUNK_SIZE - 1) / CHUNK_SIZE;

    uint8_t packet[MAX_PACKET];
    auto* hdr = reinterpret_cast<PacketHeader*>(packet);

    for (int i = 0; i < totalChunks; i++) {
        int offset = i * CHUNK_SIZE;
        int size = std::min(CHUNK_SIZE, totalBytes - offset);

        hdr->frameId = frameId;
        hdr->chunkIndex = static_cast<uint16_t>(i);
        hdr->totalChunks = static_cast<uint16_t>(totalChunks);

        std::memcpy(packet + HEADER_SIZE, data + offset, size);

        sendto(sock,
               reinterpret_cast<const char*>(packet),
               HEADER_SIZE + size,
               0,
               reinterpret_cast<sockaddr*>(&clientAddr),
               clientLen);
    }

    frameId++;
}

void ScreenStreamer::listenLoop() {
    int recvSock = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5000); // discovery + control
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(recvSock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("bind failed");
        return;
    }

    char buffer[256];
    sockaddr_in client{};
    socklen_t len = sizeof(client);

    std::cout << "Server in ascolto su porta 5000...\n";

    while (true) {
        int r = recvfrom(recvSock, buffer, sizeof(buffer) - 1, 0,
                         (sockaddr*)&client, &len);

        if (r <= 0) continue;

        buffer[r] = '\0';
        std::string msg(buffer);

        std::cout << "Ricevuto: " << msg
                  << " da " << inet_ntoa(client.sin_addr)
                  << ":" << ntohs(client.sin_port) << "\n";

        // 🔍 DISCOVERY
        if (msg == "DISCOVER_SERVER") {
            const char* reply = "SERVER_HERE";

            sendto(recvSock, reply, strlen(reply), 0,
                   (sockaddr*)&client, len);

            std::cout << "Risposto a discovery\n";
        }

        // 🎬 START STREAM
        else if (msg.rfind("START_STREAM:", 0) == 0) {
            int port = std::stoi(msg.substr(13));

            clientAddr = client;
            clientAddr.sin_port = htons(port);
            clientLen = len;

            clientReady = true;

            std::cout << "Streaming verso "
                      << inet_ntoa(clientAddr.sin_addr)
                      << ":" << ntohs(clientAddr.sin_port)
                      << "\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}