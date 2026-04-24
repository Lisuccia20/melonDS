#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <stdint.h>
#include <chrono>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define closesocket closesocket
  typedef int socklen_t;
  typedef SOCKET socket_t;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int socket_t;
  #define closesocket close
#endif

class EmuInstance;

class ScreenStreamer {
public:
    ScreenStreamer(uint16_t port, EmuInstance* emu);
    ~ScreenStreamer();

    void sendFrame(const void* bgraData, int width, int height);
    void handleOffer(const std::string& sdp);

    socket_t sock        = -1;
    bool answer_pending  = false;

    sockaddr_in clientAddr{};
    socklen_t   clientLen = sizeof(sockaddr_in);
    bool        touchActive = false;

    std::atomic<bool>     touching{false};
    std::atomic<uint16_t> touchX{0};
    std::atomic<uint16_t> touchY{0};
    std::chrono::steady_clock::time_point lastTouchDown;

private:
    void initGStreamer(uint16_t port);

    void handleTouch(uint8_t type, uint16_t x, uint16_t y);
    void handleButton(uint8_t type, uint8_t id, int8_t value);

    EmuInstance*        emuInstance = nullptr;
    std::atomic<bool>   running{true};

    socket_t touchSock = -1;
    socket_t inputSock = -1;

    int frameId = 0;
};