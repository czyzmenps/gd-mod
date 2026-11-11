#pragma once

// The party server, built into the mod.
//
// When somebody presses "Start party" and no party server is set in the Config,
// their own game becomes the server for that one party: it listens on a TCP
// port, the host's own mod connects to it through localhost and the friends
// connect to it from the internet. It speaks exactly the same protocol as the
// stand-alone server in server/server.js (without saved rooms and without
// invites), so the rest of the mod does not care which one it talks to.
//
// Like Net.hpp this deliberately does not include any Geode / GD headers.

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace collab {

    class LocalServer {
    public:
        LocalServer();
        ~LocalServer();
        LocalServer(LocalServer const&) = delete;
        LocalServer& operator=(LocalServer const&) = delete;

        // Starts listening in the background on firstPort, or on one of the
        // next few ports when that one is taken. Returns the port, or 0 and a
        // readable reason in `error`.
        int start(int firstPort, std::string& error);

        // Tells everybody the party is over and stops. Safe to call twice.
        void stop();

        bool running() const { return m_running.load(); }
        int port() const { return m_port; }

    private:
        struct State;
        void run();

        std::unique_ptr<State> m_state;
        std::thread m_thread;
        std::atomic_bool m_stop{false};
        std::atomic_bool m_running{false};
        int m_port = 0;
    };

}
