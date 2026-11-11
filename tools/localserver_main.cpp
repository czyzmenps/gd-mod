// Runs the mod's built-in party server on its own (Linux / Windows) so that it
// can be tested with tools/test-local.js:  see that file.
//   localserver_main [port]      stops on SIGINT / SIGTERM
#include "../src/LocalServer.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

static std::atomic_bool g_quit{false};

int main(int argc, char** argv) {
    int port = argc > 1 ? std::atoi(argv[1]) : 5588;
    std::signal(SIGINT, [](int) { g_quit = true; });
    std::signal(SIGTERM, [](int) { g_quit = true; });

    collab::LocalServer server;
    std::string error;
    int got = server.start(port, error);
    if (got == 0) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("listening %d\n", got);
    std::fflush(stdout);
    while (!g_quit) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.stop();
    return 0;
}
