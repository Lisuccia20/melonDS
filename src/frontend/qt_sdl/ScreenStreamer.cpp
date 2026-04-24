#include "ScreenStreamer.h"
#include "EmuInstance.h"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>

#include <thread>
#include <iostream>
#include <vector>
#include <cstring>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  #define close(s)   closesocket(s)
  #define socklen_t  int
  // mDNS stub
  typedef void* DNSServiceRef;
  #define kDNSServiceInterfaceIndexAny 0
  static DNSServiceRef serviceRef = nullptr;
  static inline void DNSServiceRegister(DNSServiceRef*, int, int,
      const char*, const char*, void*, void*,
      uint16_t, int, const void*, void*, void*) {}
  static inline void DNSServiceRefDeallocate(DNSServiceRef) {}
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <dns_sd.h>
  static DNSServiceRef serviceRef = nullptr;
#endif

/* ---------------- PACKETS ---------------- */

#pragma pack(push, 1)
struct TouchPacket {
    uint8_t type;
    uint16_t x;
    uint16_t y;
    uint32_t ts;
};
#pragma pack(pop)

#pragma pack(push, 1)
struct ButtonPacket {
    uint8_t type;
    uint8_t id;
    int8_t  value;
    uint8_t padding;
};
#pragma pack(pop)

/* ---------------- GLOBAL GST ---------------- */

static GstElement* pipeline  = nullptr;
static GstElement* appsrc    = nullptr;
static GstElement* webrtcbin = nullptr;
static GMainLoop*  gst_loop  = nullptr;

/* ---------------- IP ---------------- */

static std::string getLocalIP() {
    char buf[INET_ADDRSTRLEN] = "0.0.0.0";

#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return buf;
#else
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return buf;
#endif

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);

    if (connect(s, (sockaddr*)&dst, sizeof(dst)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (getsockname(s, (sockaddr*)&local, &len) == 0) {
            inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
        }
    }

    close(s);
    return buf;
}

/* ---------------- mDNS ---------------- */

// Rimuovi:
//   #include <dns_sd.h>
//   static DNSServiceRef serviceRef = nullptr;
//   e la funzione startMDNS()

/* ---------------- UDP BROADCAST DISCOVERY ---------------- */

static std::atomic<bool> discoveryRunning{false};

static void startDiscovery(uint16_t sigPort) {
    discoveryRunning = true;

    std::thread([sigPort] {
#ifdef _WIN32
        SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
#else
        int s = socket(AF_INET, SOCK_DGRAM, 0);
#endif
        if (s < 0) { std::cerr << "[Discovery] socket failed\n"; return; }

        int yes = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
#ifdef SO_REUSEPORT
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

        sockaddr_in local{};
        local.sin_family      = AF_INET;
        local.sin_port        = htons(5000);
        local.sin_addr.s_addr = INADDR_ANY;

        if (bind(s, (sockaddr*)&local, sizeof(local)) < 0) {
            std::cerr << "[Discovery] bind failed\n";
            close(s); return;
        }

        std::cout << "[Discovery] listening on :5000\n";

        char buf[64];
        sockaddr_in client{};
        socklen_t clientLen = sizeof(client);

        while (discoveryRunning) {
            // timeout ogni secondo per poter uscire
#ifdef _WIN32
            DWORD tv = 1000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
#else
            timeval tv{1, 0};
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
            int n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                             (sockaddr*)&client, &clientLen);
            if (n <= 0) continue;

            buf[n] = '\0';
            std::string msg(buf, n);

            // trim whitespace/newline
            msg.erase(msg.find_last_not_of(" \r\n\t") + 1);

            if (msg == "WBRT_DISCOVER") {
                std::cout << "[Discovery] probe from "
                          << inet_ntoa(client.sin_addr) << "\n";
                const char* reply = "WBRT_HERE";
                sendto(s, reply, (int)strlen(reply), 0,
                       (sockaddr*)&client, clientLen);
            }
        }

        close(s);
    }).detach();
}

static void stopDiscovery() {
    discoveryRunning = false;
}

/* ---------------- ICE ---------------- */

struct IceData {
    int mline;
    std::string candidate;
};

static gboolean add_ice_safe(gpointer user_data) {
    auto* d = static_cast<IceData*>(user_data);
    g_signal_emit_by_name(webrtcbin, "add-ice-candidate",
        (guint)d->mline, d->candidate.c_str());
    delete d;
    return FALSE;
}

/* ---------------- ICE SEND ---------------- */

static void on_ice_candidate(GstElement*, guint mline, gchar* candidate, gpointer ud) {
    auto* self = static_cast<ScreenStreamer*>(ud);
    if (!candidate || self->sock < 0) return;

    std::string msg = "ICE|" + std::to_string(mline) + "|0|" + candidate;

    sendto(self->sock, msg.data(), (int)msg.size(), 0,
           (sockaddr*)&self->clientAddr, self->clientLen);
}

/* ---------------- SDP ANSWER ---------------- */

static void on_answer_created(GstPromise* promise, gpointer ud) {
    auto* self = static_cast<ScreenStreamer*>(ud);

    const GstStructure* reply = gst_promise_get_reply(promise);
    if (!reply) return;

    GstWebRTCSessionDescription* answer = nullptr;
    gst_structure_get(reply,
        "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, nullptr);

    if (!answer) return;

    GstPromise* lp = gst_promise_new();
    g_signal_emit_by_name(webrtcbin, "set-local-description", answer, lp);
    gst_promise_interrupt(lp);
    gst_promise_unref(lp);

    gchar* sdp = gst_sdp_message_as_text(answer->sdp);
    if (sdp) {
        std::vector<uint8_t> buf(4 + strlen(sdp));
        memcpy(buf.data(), "RBWB", 4);
        memcpy(buf.data() + 4, sdp, strlen(sdp));

        sendto(self->sock, (const char*)buf.data(), (int)buf.size(), 0,
               (sockaddr*)&self->clientAddr, self->clientLen);

        g_free(sdp);
    }

    gst_webrtc_session_description_free(answer);
    gst_promise_unref(promise);

    self->answer_pending = false;
}

/* ---------------- OFFER ---------------- */

static void on_remote_desc_set(GstPromise* promise, gpointer ud) {
    auto* self = static_cast<ScreenStreamer*>(ud);
    gst_promise_unref(promise);

    GstPromise* ap = gst_promise_new_with_change_func(
        on_answer_created, self, nullptr);

    g_signal_emit_by_name(webrtcbin, "create-answer", nullptr, ap);
}

void ScreenStreamer::handleOffer(const std::string& sdp) {
    if (answer_pending) return;

    GstSDPMessage* msg = nullptr;
    if (gst_sdp_message_new_from_text(sdp.c_str(), &msg) != GST_SDP_OK)
        return;

    auto* offer = gst_webrtc_session_description_new(
        GST_WEBRTC_SDP_TYPE_OFFER, msg);

    answer_pending = true;

    GstPromise* p = gst_promise_new_with_change_func(
        on_remote_desc_set, this, nullptr);

    g_signal_emit_by_name(webrtcbin, "set-remote-description", offer, p);
    gst_webrtc_session_description_free(offer);
}

/* ---------------- GST INIT ---------------- */

void ScreenStreamer::initGStreamer(uint16_t port)
{
    gst_init(nullptr, nullptr);

    gst_loop = g_main_loop_new(nullptr, FALSE);
    std::thread([] {
        g_main_loop_run(gst_loop);
    }).detach();

    pipeline  = gst_pipeline_new("streamer");

    appsrc     = gst_element_factory_make("appsrc", "appsrc");
    auto* conv = gst_element_factory_make("videoconvert", "conv");
    auto* capsfilter = gst_element_factory_make("capsfilter", "cf");
    GstCaps* outcaps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "I420",
        nullptr);

    g_object_set(capsfilter, "caps", outcaps, nullptr);
    auto* enc  = gst_element_factory_make("x264enc", "enc");
    auto* pay  = gst_element_factory_make("rtph264pay", "pay");
    webrtcbin  = gst_element_factory_make("webrtcbin", "webrtc");

    if (!pipeline || !appsrc || !conv || !enc || !pay || !webrtcbin)
    {
        std::cerr << "[GST] ERROR creating elements\n";
        return;
    }

    GstCaps* caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "BGRA",
        "width",  G_TYPE_INT, 256,
        "height", G_TYPE_INT, 192,
        "framerate", GST_TYPE_FRACTION, 60, 1,
        nullptr);

    g_object_set(appsrc,
        "caps", caps,
        "is-live", TRUE,
        "format", GST_FORMAT_TIME,
        "do-timestamp", TRUE,
        nullptr);

    gst_caps_unref(caps);

    g_object_set(enc,
        "tune", 0x00000004,
        "speed-preset", 1,
        "bitrate", 1500,
        "key-int-max", 30,
        "bframes", 0,
        "threads", 1,
        "sync-lookahead", 0,
        "byte-stream", TRUE,
        nullptr);

    g_object_set(pay,
        "config-interval", 1,
        "pt", 96,
        nullptr);

    g_object_set(webrtcbin,
        "stun-server", "stun:stun.l.google.com:19302",
        "latency", 50,
        nullptr);

    gst_bin_add_many(GST_BIN(pipeline),
        appsrc, conv, enc, pay, webrtcbin, nullptr);

    if (!gst_element_link_many(appsrc, conv, enc, pay, nullptr))
    {
        std::cerr << "[GST] ERROR linking video pipeline\n";
    }

    GstPad* srcpad = gst_element_get_static_pad(pay, "src");
    GstPad* sinkpad = gst_element_request_pad_simple(webrtcbin, "sink_%u");

    if (gst_pad_link(srcpad, sinkpad) != GST_PAD_LINK_OK)
    {
        std::cerr << "[GST] ERROR linking RTP to WebRTC\n";
    }

    gst_object_unref(srcpad);
    gst_object_unref(sinkpad);

    g_signal_connect(webrtcbin, "on-ice-candidate",
        G_CALLBACK(on_ice_candidate), this);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    std::cout << "[GST] Pipeline started OK\n";
}

/* ---------------- TOUCH ---------------- */

void ScreenStreamer::handleTouch(uint8_t type, uint16_t x, uint16_t y) {
    if (!emuInstance) return;

    if (type == 2) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastTouchDown).count();
        if (elapsed < 50)
        {
            std::thread([this]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                emuInstance->remoteTouchUp();
            }).detach();
        }
        else
        {
            emuInstance->remoteTouchUp();
        }
        return;
    }

    uint16_t cx = std::min<uint16_t>(x, 255);
    uint16_t cy = std::min<uint16_t>(y, 191);

    if (type == 0) {
        lastTouchDown = std::chrono::steady_clock::now();
        emuInstance->remoteTouchDown(cx, cy);
    } else {
        emuInstance->remoteTouchMove(cx, cy);
    }
}

/* ---------------- BUTTONS ---------------- */

void ScreenStreamer::handleButton(uint8_t type, uint8_t id, int8_t value)
{
    if (!emuInstance) return;
    if (type != 0) return;

    bool pressed = (value != 0);

    static const int toMelonDS[] = {
         0,  // a      → A
         1,  // b      → B
        10,  // x      → X
        11,  // y      → Y
         9,  // lb     → L
         8,  // rb     → R
        -1,  // lt     → n/a
        -1,  // rt     → n/a
         2,  // back   → Select
         3,  // start  → Start
        -1,  // ls     → n/a
        -1,  // rs     → n/a
         6,  // dpadUp    → Up
         7,  // dpadDown  → Down
         5,  // dpadLeft  → Left
         4,  // dpadRight → Right
    };

    if (id < sizeof(toMelonDS)/sizeof(toMelonDS[0]) && toMelonDS[id] != -1)
        emuInstance->remoteButtonState(toMelonDS[id], pressed);
}

/* ---------------- FRAME ---------------- */

void ScreenStreamer::sendFrame(const void* data, int w, int h)
{
    if (!appsrc || !data) return;

    static GstClockTime baseTime = gst_util_get_timestamp();

    int size = w * h * 4;

    GstBuffer* buf = gst_buffer_new_allocate(nullptr, size, nullptr);
    gst_buffer_fill(buf, 0, data, size);

    GstClockTime now = gst_util_get_timestamp() - baseTime;

    GST_BUFFER_PTS(buf)      = now;
    GST_BUFFER_DTS(buf)      = now;
    GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(1, GST_SECOND, 60);

    GST_BUFFER_FLAG_UNSET(buf, GST_BUFFER_FLAG_DELTA_UNIT);

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buf);

    if (ret != GST_FLOW_OK)
    {
        std::cerr << "[GST] frame drop: " << ret << "\n";
    }
}

/* ---------------- CONSTRUCTOR ---------------- */

ScreenStreamer::ScreenStreamer(uint16_t port, EmuInstance* emu)
    : emuInstance(emu),
      running(true),
      sock(-1),
      touchSock(-1),
      inputSock(-1),
      answer_pending(false),
      touchActive(false),
      touchX(0),
      touchY(0),
      clientLen(sizeof(clientAddr))
{
    memset(&clientAddr, 0, sizeof(clientAddr));

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    // SIGNALING SOCKET
    sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        std::cerr << "[ERR] signalling socket() failed\n";

    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_port        = htons(port);
    local.sin_addr.s_addr = INADDR_ANY;

    if (bind((SOCKET)sock, (sockaddr*)&local, sizeof(local)) < 0)
        std::cerr << "[ERR] bind signalling failed\n";

    // TOUCH SOCKET
    touchSock = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (touchSock < 0)
        std::cerr << "[ERR] touch socket() failed\n";

    sockaddr_in touchLocal{};
    touchLocal.sin_family      = AF_INET;
    touchLocal.sin_port        = htons(5002);
    touchLocal.sin_addr.s_addr = INADDR_ANY;

    if (bind((SOCKET)touchSock, (sockaddr*)&touchLocal, sizeof(touchLocal)) < 0)
        std::cerr << "[ERR] bind touch failed\n";

    // INPUT SOCKET
    inputSock = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (inputSock < 0)
        std::cerr << "[ERR] input socket() failed\n";

    sockaddr_in inputLocal{};
    inputLocal.sin_family      = AF_INET;
    inputLocal.sin_port        = htons(5003);
    inputLocal.sin_addr.s_addr = INADDR_ANY;

    if (bind((SOCKET)inputSock, (sockaddr*)&inputLocal, sizeof(inputLocal)) < 0)
    {
        std::cerr << "[ERR] bind input failed\n";
    }

    // mDNS
    startDiscovery(port);

    // GSTREAMER
    initGStreamer(port);

    // THREAD: SDP + ICE
    std::thread([this] {
        char buf[65536];

        while (running)
        {
            int n = recvfrom((SOCKET)sock, buf, sizeof(buf), 0,
                             (sockaddr*)&clientAddr, &clientLen);
            if (n <= 0) continue;

            if (n > 4 && memcmp(buf, "WBRT", 4) == 0)
            {
                handleOffer(std::string(buf + 4, n - 4));
                continue;
            }

            if (n > 4 && memcmp(buf, "ICE|", 4) == 0)
            {
                std::string msg(buf, n);
                auto payload = msg.substr(4);

                auto s1 = payload.find('|');
                auto s2 = payload.find('|', s1 + 1);
                if (s1 == std::string::npos || s2 == std::string::npos)
                    continue;

                int mline = std::stoi(payload.substr(0, s1));
                std::string cand = payload.substr(s2 + 1);

                auto* d = new IceData{mline, cand};

                g_main_context_invoke(
                    g_main_loop_get_context(gst_loop),
                    add_ice_safe,
                    d);
            }
        }
    }).detach();

    // THREAD: TOUCH INPUT
    std::thread([this] {
        uint8_t buf[sizeof(TouchPacket)];

        while (running)
        {
            int n = recv((SOCKET)touchSock, (char*)buf, sizeof(buf), 0);

            if (n < (int)sizeof(TouchPacket))
                continue;

            const TouchPacket* pkt =
                reinterpret_cast<const TouchPacket*>(buf);

            handleTouch(pkt->type, pkt->x, pkt->y);
        }
    }).detach();

    // THREAD: BUTTON + AXIS INPUT
    std::thread([this] {
        uint8_t buf[256];

        std::cout << "[INPUT] button thread started\n";

        while (running)
        {
            int n = recv((SOCKET)inputSock, (char*)buf, sizeof(buf), 0);

            if (n < 0)
            {
                std::cerr << "[INPUT] recv error\n";
                break;
            }

            if (n != 4)
            {
                std::cout << "[INPUT] ignored packet size: " << n << std::endl;
                continue;
            }

            ButtonPacket pkt;
            memcpy(&pkt, buf, 4);

            std::cout << "[INPUT] type=" << (int)pkt.type
                      << " id="          << (int)pkt.id
                      << " value="       << (int)pkt.value << std::endl;

            handleButton(pkt.type, pkt.id, pkt.value);
        }
    }).detach();

    std::cout << "[ScreenStreamer] initialized OK\n";
}

/* ---------------- DESTRUCTOR ---------------- */

ScreenStreamer::~ScreenStreamer() {
    running = false;

    if (sock >= 0)      closesocket((SOCKET)sock);
    if (touchSock >= 0) closesocket((SOCKET)touchSock);
    if (inputSock >= 0) closesocket((SOCKET)inputSock);

    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }

    if (gst_loop) {
        g_main_loop_quit(gst_loop);
        g_main_loop_unref(gst_loop);
    }

    stopDiscovery();

#ifdef _WIN32
    WSACleanup();
#endif
}