#pragma once

#include <string>
#include <cstdint>
#include <atomic>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
#endif

class ScreenStreamer {
public:
    ScreenStreamer(uint16_t port);
    ~ScreenStreamer();

    void sendFrame(const void* bgraData, int width, int height);

private:
    void listenLoop();   // ✔ nome unico e coerente

    int sock = -1;
    uint32_t frameId = 0;

    sockaddr_in clientAddr{};
    std::atomic<bool> clientReady{false};

    static constexpr int CHUNK_PAYLOAD = 60000;
};