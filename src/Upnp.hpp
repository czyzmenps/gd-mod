#pragma once

// Asks the router to forward a TCP port to this computer (UPnP) and finds out
// the public address, so friends on the internet can reach the party server
// that runs inside the mod. Windows only (uses the built-in UPnP support of
// Windows); on other systems it just reports that it is not available.
//
// Like Net.hpp this deliberately does not include any Geode / GD headers.

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace collab {

    struct UpnpResult {
        bool ok = false;
        std::string address; // public IP address when ok
        std::string error;   // short problem description when not ok
        std::string hint;    // what the player can do about it
    };

    class PortMapper {
    public:
        PortMapper() = default;
        ~PortMapper();
        PortMapper(PortMapper const&) = delete;
        PortMapper& operator=(PortMapper const&) = delete;

        // Starts working in the background and returns immediately.
        void open(int port);

        // Removes the forwarding again. Returns immediately.
        void close();

        // true after open() until there is a result (success or failure)
        bool working() const;
        // true between open() and close()
        bool started() const { return m_shared != nullptr; }
        UpnpResult result() const;

    private:
        struct Shared {
            std::mutex mutex;
            std::condition_variable cv;
            bool stop = false;
            bool done = false;
            UpnpResult result;
        };
        static void work(std::shared_ptr<Shared> shared, int port);

        std::shared_ptr<Shared> m_shared;
        std::thread m_thread;
    };

}
