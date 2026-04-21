#include "ScreenStreamer.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t frameId;
    uint16_t chunkIndex;
    uint16_t totalChunks;
};
#pragma pack(pop)

static constexpr int HEADER_SIZE  = sizeof(PacketHeader);
static constexpr int CHUNK_PAYLOAD = 60000;
static constexpr int MAX_PACKET    = HEADER_SIZE + CHUNK_PAYLOAD;

ScreenStreamer::ScreenStreamer(const std::string& ip, uint16_t port) {
    sock = socket(AF_INET, SOCK_DGRAM, 0);

    int bufSize = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
}

ScreenStreamer::~ScreenStreamer() {
    close(sock);
}

void ScreenStreamer::sendFrame(const void* bgraData, int width, int height) {
    const uint8_t* data   = reinterpret_cast<const uint8_t*>(bgraData);
    int totalBytes        = width * height * 4;
    int totalChunks       = (totalBytes + CHUNK_PAYLOAD - 1) / CHUNK_PAYLOAD;

    uint8_t packet[MAX_PACKET];
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(packet);

    for (int i = 0; i < totalChunks; i++) {
        int offset      = i * CHUNK_PAYLOAD;
        int payloadSize = std::min(CHUNK_PAYLOAD, totalBytes - offset);

        hdr->frameId      = frameId;
        hdr->chunkIndex   = static_cast<uint16_t>(i);
        hdr->totalChunks  = static_cast<uint16_t>(totalChunks);

        memcpy(packet + HEADER_SIZE, data + offset, payloadSize);

        sendto(sock, packet, HEADER_SIZE + payloadSize, 0,
               reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    }

    frameId++;
}