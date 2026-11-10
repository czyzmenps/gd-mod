#pragma once

// Tiny asynchronous TCP line client. It deliberately does not include any
// Geode / GD headers so that it can use the platform socket headers freely.
//
// Protocol framing: UTF-8 text, one JSON message per line ('\n' terminated).

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace collab {

    struct NetEvent {
        enum class Type { Connected, Line, Closed };
        Type type;
        std::string text; // the received line, or the close reason
    };

    class NetClient {
    public:
        NetClient() = default;
        ~NetClient();
        NetClient(NetClient const&) = delete;
        NetClient& operator=(NetClient const&) = delete;

        // Starts connecting in the background. Returns immediately.
        void connect(std::string host, int port);

        // Thread-safe. Queues one message line (a '\n' is appended).
        void send(std::string const& line);

        // Stops the background thread (blocks for a few hundred ms at most).
        // No Closed event is produced for a close requested this way.
        void close();

        // Moves all pending events to the caller. Call from the main thread.
        std::vector<NetEvent> poll();

    private:
        void run(std::string host, int port);
        void push(NetEvent::Type type, std::string text = {});

        std::thread m_thread;
        std::atomic_bool m_stop{false};
        std::mutex m_mutex;
        std::deque<NetEvent> m_events;
        std::deque<std::string> m_outgoing;
    };

}
