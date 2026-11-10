#include "Net.hpp"

#include <chrono>
#include <cstring>
#include <mutex>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using sock_t = SOCKET;
    static constexpr sock_t kInvalidSock = INVALID_SOCKET;
    static void closeSock(sock_t s) { ::closesocket(s); }
    static bool wouldBlock() {
        int e = ::WSAGetLastError();
        return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
    }
    static void setNonBlocking(sock_t s) {
        u_long mode = 1;
        ::ioctlsocket(s, FIONBIO, &mode);
    }
    static void ensureSocketsStarted() {
        static std::once_flag flag;
        std::call_once(flag, [] {
            WSADATA data;
            ::WSAStartup(MAKEWORD(2, 2), &data);
        });
    }
#else
    #include <errno.h>
    #include <fcntl.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <unistd.h>
    using sock_t = int;
    static constexpr sock_t kInvalidSock = -1;
    static void closeSock(sock_t s) { ::close(s); }
    static bool wouldBlock() {
        return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EINTR;
    }
    static void setNonBlocking(sock_t s) {
        int flags = ::fcntl(s, F_GETFL, 0);
        ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
    }
    static void ensureSocketsStarted() {}
#endif

namespace collab {

    namespace {
        // Waits until the socket is readable/writable. Returns >0 when ready.
        int waitSocket(sock_t s, bool wantWrite, int timeoutMs) {
            fd_set rd, wr;
            FD_ZERO(&rd);
            FD_ZERO(&wr);
            if (wantWrite) FD_SET(s, &wr);
            else FD_SET(s, &rd);
            timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
            return ::select(0, &rd, &wr, nullptr, &tv);
#else
            return ::select(s + 1, &rd, &wr, nullptr, &tv);
#endif
        }
    }

    NetClient::~NetClient() {
        this->close();
    }

    void NetClient::connect(std::string host, int port) {
        this->close();
        {
            std::lock_guard lock(m_mutex);
            m_events.clear();
            m_outgoing.clear();
        }
        m_stop = false;
        m_thread = std::thread([this, host = std::move(host), port] {
            this->run(host, port);
        });
    }

    void NetClient::send(std::string const& line) {
        std::lock_guard lock(m_mutex);
        m_outgoing.push_back(line + "\n");
    }

    void NetClient::close() {
        m_stop = true;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    std::vector<NetEvent> NetClient::poll() {
        std::vector<NetEvent> out;
        std::lock_guard lock(m_mutex);
        out.reserve(m_events.size());
        while (!m_events.empty()) {
            out.push_back(std::move(m_events.front()));
            m_events.pop_front();
        }
        return out;
    }

    void NetClient::push(NetEvent::Type type, std::string text) {
        std::lock_guard lock(m_mutex);
        m_events.push_back(NetEvent{type, std::move(text)});
    }

    void NetClient::run(std::string host, int port) {
        ensureSocketsStarted();

        // ---- resolve + connect (with a timeout) ----
        addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo* result = nullptr;
        if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0 || !result) {
            if (!m_stop) this->push(NetEvent::Type::Closed, "Could not find the server address");
            return;
        }

        sock_t sock = kInvalidSock;
        for (addrinfo* ai = result; ai && sock == kInvalidSock && !m_stop; ai = ai->ai_next) {
            sock_t s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s == kInvalidSock) continue;
            setNonBlocking(s);

            int rc = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
            bool ok = (rc == 0);
            if (!ok && wouldBlock()) {
                // wait up to ~8 seconds, in slices so close() stays responsive
                for (int i = 0; i < 80 && !m_stop; i++) {
                    int ready = waitSocket(s, true, 100);
                    if (ready > 0) {
                        int err = 0;
#ifdef _WIN32
                        int len = sizeof(err);
                        ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
#else
                        socklen_t len = sizeof(err);
                        ::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
                        ok = (err == 0);
                        break;
                    }
                    if (ready < 0) break;
                }
            }
            if (ok) sock = s;
            else closeSock(s);
        }
        ::freeaddrinfo(result);

        if (m_stop) {
            if (sock != kInvalidSock) closeSock(sock);
            return;
        }
        if (sock == kInvalidSock) {
            this->push(NetEvent::Type::Closed, "Could not connect to the server");
            return;
        }

        int one = 1;
        ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&one), sizeof(one));
        this->push(NetEvent::Type::Connected);

        // ---- main loop ----
        std::string inbuf;
        std::string outbuf;
        size_t outPos = 0;
        std::string closeReason;
        char chunk[64 * 1024];

        while (!m_stop && closeReason.empty()) {
            // move queued messages into the pending output buffer
            {
                std::lock_guard lock(m_mutex);
                while (!m_outgoing.empty()) {
                    outbuf += m_outgoing.front();
                    m_outgoing.pop_front();
                }
            }

            // write as much as the socket accepts
            while (outPos < outbuf.size()) {
                int n = static_cast<int>(::send(
                    sock, outbuf.data() + outPos, static_cast<int>(outbuf.size() - outPos), 0
                ));
                if (n > 0) {
                    outPos += static_cast<size_t>(n);
                }
                else if (n < 0 && wouldBlock()) {
                    break;
                }
                else {
                    closeReason = "Connection lost while sending";
                    break;
                }
            }
            if (outPos >= outbuf.size()) {
                outbuf.clear();
                outPos = 0;
            }
            if (!closeReason.empty()) break;

            // wait for incoming data (short slices so outgoing messages stay snappy)
            int ready = waitSocket(sock, false, 30);
            if (ready < 0) {
                closeReason = "Connection error";
                break;
            }
            if (ready == 0) continue;

            int n = static_cast<int>(::recv(sock, chunk, sizeof(chunk), 0));
            if (n > 0) {
                inbuf.append(chunk, static_cast<size_t>(n));
                size_t start = 0;
                while (true) {
                    size_t nl = inbuf.find('\n', start);
                    if (nl == std::string::npos) break;
                    if (nl > start) {
                        size_t end = nl;
                        if (inbuf[end - 1] == '\r') end--;
                        if (end > start) this->push(NetEvent::Type::Line, inbuf.substr(start, end - start));
                    }
                    start = nl + 1;
                }
                inbuf.erase(0, start);
                if (inbuf.size() > 64u * 1024u * 1024u) {
                    closeReason = "Server sent an oversized message";
                }
            }
            else if (n == 0) {
                closeReason = "The server closed the connection";
            }
            else if (!wouldBlock()) {
                closeReason = "Connection lost";
            }
        }

        closeSock(sock);
        if (!m_stop) {
            this->push(NetEvent::Type::Closed, closeReason.empty() ? "Disconnected" : closeReason);
        }
    }

}
