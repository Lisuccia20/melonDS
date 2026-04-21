#pragma once
#include <string>
#include <cstdint>

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