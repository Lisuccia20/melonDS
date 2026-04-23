#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <stdint.h>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>

class EmuInstance;

class ScreenStreamer {
public:
    ScreenStreamer(uint16_t port, EmuInstance* emu);
    ~ScreenStreamer();

    // VIDEO
    void sendFrame(const void* bgraData, int width, int height);

    // WEBRTC
    void handleOffer(const std::string& sdp);

    // SOCKET principale (signaling + ICE)
    int sock = -1;
    bool answer_pending = false;

    // CLIENT remoto (serve per ICE + SDP)
    sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(sockaddr_in);
    bool touchActive = false;

    /* ---------------- STATE ---------------- */
    std::atomic<bool> touching{false};
    std::atomic<uint16_t> touchX{0};
    std::atomic<uint16_t> touchY{0};
    std::chrono::steady_clock::time_point lastTouchDown;


private:
    /* ---------------- GStreamer ---------------- */
    void initGStreamer(uint16_t port);

    /* ---------------- NETWORK ---------------- */
    void startListening();        // WBRT + ICE
    void startTouchListening();   // touch UDP
    void startButtonListening();  // gamepad UDP

    /* ---------------- INPUT HANDLING ---------------- */
    void handleTouchPacket(const uint8_t* data, ssize_t n);
    void handleButtonPacket(const uint8_t* data, ssize_t n);

    /* ---------------- INJECTION (melonDS) ---------------- */
    void injectTouchDown(uint16_t x, uint16_t y);
    void injectTouchMove(uint16_t x, uint16_t y);
    void injectTouchUp();

    void injectButtonState(uint8_t key, bool pressed);
    void handleTouch(uint8_t type, uint16_t x, uint16_t y);
    void handleButton(uint8_t type, uint8_t id, int8_t value);

private:
    EmuInstance* emuInstance = nullptr;

    std::atomic<bool> running{true};

    std::thread listenThread;
    std::thread touchThread;
    std::thread buttonThread;

    /* ---------------- SOCKETS ---------------- */
    int touchSock  = -1;
    int inputSock = -1;

    int frameId = 0;
};