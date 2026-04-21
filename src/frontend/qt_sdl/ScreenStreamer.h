#pragma once

#include <string>
#include <cstdint>

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
    ScreenStreamer(const std::string& ip, uint16_t port);
    ~ScreenStreamer();

    void sendFrame(const void* bgraData, int width, int height);

private:
    int sock;

    struct sockaddr_in addr;

    uint32_t frameId = 0;

    static constexpr int CHUNK_PAYLOAD = 60000;
};