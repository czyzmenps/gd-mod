#include "Upnp.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <objbase.h>
    #include <oleauto.h>
    #include <natupnp.h>
#endif

namespace collab {

    namespace {

        // 10.x, 172.16-31.x, 192.168.x, 100.64-127.x (carrier-grade NAT), 169.254.x, 127.x, 0.x
        bool isPrivateAddress(std::string const& ip) {
            unsigned a = 0, b = 0, c = 0, d = 0;
            if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return true;
            if (a == 0 || a == 10 || a == 127) return true;
            if (a == 172 && b >= 16 && b <= 31) return true;
            if (a == 192 && b == 168) return true;
            if (a == 100 && b >= 64 && b <= 127) return true;
            if (a == 169 && b == 254) return true;
            return false;
        }

        UpnpResult failure(std::string error, std::string hint) {
            UpnpResult r;
            r.error = std::move(error);
            r.hint = std::move(hint);
            return r;
        }

        // kept short: the window shows these in two lines
        char const* kHintGeneric =
            "Turn on UPnP in your router, or enter your public address in the Config (Public address).";

    }

#ifdef _WIN32

    namespace {

        std::string toUtf8(BSTR text) {
            if (!text) return "";
            int const length = static_cast<int>(SysStringLen(text));
            if (length == 0) return "";
            int const size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), size, nullptr, nullptr);
            return out;
        }

        std::string hex(HRESULT hr) {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
            return buffer;
        }

        // the address of this computer in the local network (the adapter that has a route to the internet)
        std::string localAddress() {
            WSADATA data;
            ::WSAStartup(MAKEWORD(2, 2), &data);

            std::string result;
            SOCKET s = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (s != INVALID_SOCKET) {
                sockaddr_in remote {};
                remote.sin_family = AF_INET;
                remote.sin_port = htons(53);
                ::inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
                // for UDP, connect() only picks the route, nothing is sent
                if (::connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
                    sockaddr_in local {};
                    int length = sizeof(local);
                    if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
                        char text[INET_ADDRSTRLEN] = {};
                        ::inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text));
                        result = text;
                    }
                }
                ::closesocket(s);
            }
            return result;
        }

        // Runs everything on one thread, because the COM objects belong to it.
        UpnpResult openMapping(int port, IUPnPNAT*& nat, IStaticPortMappingCollection*& collection) {
            HRESULT hr = ::CoCreateInstance(
                __uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT), reinterpret_cast<void**>(&nat)
            );
            if (FAILED(hr) || !nat) {
                return failure("Windows UPnP is not available (" + hex(hr) + ")", kHintGeneric);
            }

            // the router is discovered in the background, which can take a few seconds
            for (int attempt = 0; attempt < 12 && !collection; attempt++) {
                hr = nat->get_StaticPortMappingCollection(&collection);
                if (FAILED(hr)) collection = nullptr;
                if (!collection) ::Sleep(700);
            }
            if (!collection) {
                return failure("Your router does not allow automatic port opening (UPnP)", kHintGeneric);
            }

            std::string const local = localAddress();
            if (local.empty()) {
                return failure("No network connection found", "Connect to the internet and try again.");
            }

            std::wstring const localWide(local.begin(), local.end());
            BSTR protocol = ::SysAllocString(L"TCP");
            BSTR client = ::SysAllocString(localWide.c_str());
            BSTR description = ::SysAllocString(L"Live Collab party");

            // an old forwarding of the same port (for example after a crash) would make Add fail
            collection->Remove(port, protocol);

            IStaticPortMapping* mapping = nullptr;
            hr = collection->Add(port, protocol, port, client, VARIANT_TRUE, description, &mapping);

            ::SysFreeString(description);
            ::SysFreeString(client);

            UpnpResult result;
            if (FAILED(hr) || !mapping) {
                ::SysFreeString(protocol);
                return failure("The router refused to open port " + std::to_string(port) + " (" + hex(hr) + ")", kHintGeneric);
            }

            BSTR external = nullptr;
            hr = mapping->get_ExternalIPAddress(&external);
            std::string const address = SUCCEEDED(hr) ? toUtf8(external) : std::string();
            if (external) ::SysFreeString(external);
            mapping->Release();
            ::SysFreeString(protocol);

            if (address.empty() || address == "0.0.0.0") {
                return failure(
                    "The router did not tell its public address",
                    "Enter your public address in the Config (Public address)."
                );
            }
            if (isPrivateAddress(address)) {
                return failure(
                    "Your internet provider shares one address between customers",
                    "Friends cannot reach this PC. Use a tunnel such as playit.gg and enter its address in the Config "
                    "(Public address)."
                );
            }

            result.ok = true;
            result.address = address;
            return result;
        }

    }

    void PortMapper::work(std::shared_ptr<Shared> shared, int port) {
        HRESULT const init = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        IUPnPNAT* nat = nullptr;
        IStaticPortMappingCollection* collection = nullptr;
        UpnpResult result = openMapping(port, nat, collection);

        {
            std::lock_guard lock(shared->mutex);
            shared->result = result;
            shared->done = true;
        }

        // keep the forwarding for as long as the party runs, then remove it again
        {
            std::unique_lock lock(shared->mutex);
            shared->cv.wait(lock, [&] { return shared->stop; });
        }
        if (result.ok && collection) {
            BSTR protocol = ::SysAllocString(L"TCP");
            collection->Remove(port, protocol);
            ::SysFreeString(protocol);
        }
        if (collection) collection->Release();
        if (nat) nat->Release();
        if (SUCCEEDED(init)) ::CoUninitialize();
    }

#else

    void PortMapper::work(std::shared_ptr<Shared> shared, int) {
        {
            std::lock_guard lock(shared->mutex);
            shared->result = failure("Automatic port opening only works on Windows", kHintGeneric);
            shared->done = true;
        }
        std::unique_lock lock(shared->mutex);
        shared->cv.wait(lock, [&] { return shared->stop; });
    }

#endif

    PortMapper::~PortMapper() {
        this->close();
        if (m_thread.joinable()) m_thread.join();
    }

    void PortMapper::open(int port) {
        this->close();
        // the previous forwarding is removed before the new one is made
        if (m_thread.joinable()) m_thread.join();

        m_shared = std::make_shared<Shared>();
        m_thread = std::thread(&PortMapper::work, m_shared, port);
    }

    void PortMapper::close() {
        if (!m_shared) return;
        {
            std::lock_guard lock(m_shared->mutex);
            m_shared->stop = true;
        }
        m_shared->cv.notify_all();
        m_shared.reset();
    }

    bool PortMapper::working() const {
        if (!m_shared) return false;
        std::lock_guard lock(m_shared->mutex);
        return !m_shared->done;
    }

    UpnpResult PortMapper::result() const {
        if (!m_shared) return {};
        std::lock_guard lock(m_shared->mutex);
        return m_shared->result;
    }

}
