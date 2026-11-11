#include "LocalServer.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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
    using sock_len_t = int;
    static constexpr int kSendFlags = 0;
#else
    #include <arpa/inet.h>
    #include <errno.h>
    #include <fcntl.h>
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
        return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR;
    }
    static void setNonBlocking(sock_t s) {
        int flags = ::fcntl(s, F_GETFL, 0);
        ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
    }
    static void ensureSocketsStarted() {}
    using sock_len_t = socklen_t;
    static constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

namespace collab {

    namespace {

        constexpr int kProtocolVersion = 1;
        constexpr size_t kMaxClients = 16;          // players in one party
        constexpr size_t kMaxConnections = 32;      // sockets, including people who never say hello
        constexpr size_t kMaxLineBytes = 32u * 1024u * 1024u;
        constexpr size_t kMaxObjectString = 256u * 1024u;
        constexpr size_t kMaxPending = 128u * 1024u * 1024u; // unsent bytes before a slow client is dropped
        constexpr size_t kSnapshotChunk = 250;
        constexpr size_t kMaxSelection = 300;
        constexpr double kIdleSeconds = 60.0;
        constexpr int kJsonDepth = 24;

        // ------------------------------------------------------------------
        // a very small JSON reader / writer (only what the protocol needs)
        // ------------------------------------------------------------------

        struct Json {
            enum class Type { Null, Bool, Number, String, Array, Object };
            Type type = Type::Null;
            bool b = false;
            double n = 0.0;
            std::string s;
            std::vector<Json> a;
            std::vector<std::pair<std::string, Json>> o;

            static Json str(std::string v) { Json j; j.type = Type::String; j.s = std::move(v); return j; }
            static Json num(double v) { Json j; j.type = Type::Number; j.n = v; return j; }
            static Json boolean(bool v) { Json j; j.type = Type::Bool; j.b = v; return j; }
            static Json array() { Json j; j.type = Type::Array; return j; }
            static Json object() { Json j; j.type = Type::Object; return j; }

            Json& add(Json v) { a.push_back(std::move(v)); return *this; }
            Json& set(std::string key, Json v) { o.emplace_back(std::move(key), std::move(v)); return *this; }

            bool isString() const { return type == Type::String; }
            bool isArray() const { return type == Type::Array; }
            bool isObject() const { return type == Type::Object; }

            Json const* get(std::string_view key) const {
                if (type != Type::Object) return nullptr;
                for (auto const& [k, v] : o) {
                    if (k == key) return &v;
                }
                return nullptr;
            }
            std::string getStr(std::string_view key, std::string const& def = "") const {
                auto v = get(key);
                return v && v->isString() ? v->s : def;
            }
        };

        class Parser {
        public:
            explicit Parser(std::string_view text) : m_t(text) {}

            bool parse(Json& out) {
                skip();
                if (!value(out, 0)) return false;
                skip();
                return m_i == m_t.size();
            }

        private:
            std::string_view m_t;
            size_t m_i = 0;

            void skip() {
                while (m_i < m_t.size() && (m_t[m_i] == ' ' || m_t[m_i] == '\t' || m_t[m_i] == '\r' || m_t[m_i] == '\n')) m_i++;
            }
            bool lit(std::string_view word) {
                if (m_t.substr(m_i, word.size()) != word) return false;
                m_i += word.size();
                return true;
            }
            static void utf8(std::string& out, uint32_t cp) {
                if (cp < 0x80) out.push_back(static_cast<char>(cp));
                else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                else if (cp < 0x10000) {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                else {
                    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
            }
            bool hex4(uint32_t& out) {
                if (m_i + 4 > m_t.size()) return false;
                out = 0;
                for (int k = 0; k < 4; k++) {
                    char c = m_t[m_i++];
                    out <<= 4;
                    if (c >= '0' && c <= '9') out |= static_cast<uint32_t>(c - '0');
                    else if (c >= 'a' && c <= 'f') out |= static_cast<uint32_t>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') out |= static_cast<uint32_t>(c - 'A' + 10);
                    else return false;
                }
                return true;
            }
            bool string(std::string& out) {
                if (m_i >= m_t.size() || m_t[m_i] != '"') return false;
                m_i++;
                while (m_i < m_t.size()) {
                    char c = m_t[m_i++];
                    if (c == '"') return true;
                    if (c != '\\') {
                        out.push_back(c);
                        continue;
                    }
                    if (m_i >= m_t.size()) return false;
                    char e = m_t[m_i++];
                    switch (e) {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        case 'u': {
                            uint32_t cp = 0;
                            if (!hex4(cp)) return false;
                            if (cp >= 0xD800 && cp <= 0xDBFF) {
                                // surrogate pair
                                uint32_t low = 0;
                                if (m_t.substr(m_i, 2) != "\\u") return false;
                                m_i += 2;
                                if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            }
                            utf8(out, cp);
                        } break;
                        default: return false;
                    }
                }
                return false;
            }
            bool value(Json& out, int depth) {
                if (depth > kJsonDepth || m_i >= m_t.size()) return false;
                char c = m_t[m_i];
                if (c == '{') {
                    m_i++;
                    out.type = Json::Type::Object;
                    skip();
                    if (m_i < m_t.size() && m_t[m_i] == '}') { m_i++; return true; }
                    while (true) {
                        skip();
                        std::string key;
                        if (!string(key)) return false;
                        skip();
                        if (m_i >= m_t.size() || m_t[m_i++] != ':') return false;
                        skip();
                        Json v;
                        if (!value(v, depth + 1)) return false;
                        out.o.emplace_back(std::move(key), std::move(v));
                        skip();
                        if (m_i >= m_t.size()) return false;
                        char d = m_t[m_i++];
                        if (d == '}') return true;
                        if (d != ',') return false;
                    }
                }
                if (c == '[') {
                    m_i++;
                    out.type = Json::Type::Array;
                    skip();
                    if (m_i < m_t.size() && m_t[m_i] == ']') { m_i++; return true; }
                    while (true) {
                        skip();
                        Json v;
                        if (!value(v, depth + 1)) return false;
                        out.a.push_back(std::move(v));
                        skip();
                        if (m_i >= m_t.size()) return false;
                        char d = m_t[m_i++];
                        if (d == ']') return true;
                        if (d != ',') return false;
                    }
                }
                if (c == '"') {
                    out.type = Json::Type::String;
                    return string(out.s);
                }
                if (c == 't') { out.type = Json::Type::Bool; out.b = true; return lit("true"); }
                if (c == 'f') { out.type = Json::Type::Bool; out.b = false; return lit("false"); }
                if (c == 'n') { out.type = Json::Type::Null; return lit("null"); }
                // number
                size_t start = m_i;
                while (m_i < m_t.size()) {
                    char d = m_t[m_i];
                    if ((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.' || d == 'e' || d == 'E') m_i++;
                    else break;
                }
                if (m_i == start) return false;
                std::string text(m_t.substr(start, m_i - start));
                char* end = nullptr;
                out.n = std::strtod(text.c_str(), &end);
                if (end != text.c_str() + text.size()) return false;
                out.type = Json::Type::Number;
                return true;
            }
        };

        void dumpString(std::string& out, std::string const& s) {
            out.push_back('"');
            for (unsigned char c : s) {
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (c < 0x20) {
                            static char const* hex = "0123456789abcdef";
                            out += "\\u00";
                            out.push_back(hex[c >> 4]);
                            out.push_back(hex[c & 15]);
                        }
                        else out.push_back(static_cast<char>(c));
                }
            }
            out.push_back('"');
        }

        void dump(std::string& out, Json const& j) {
            switch (j.type) {
                case Json::Type::Null: out += "null"; break;
                case Json::Type::Bool: out += j.b ? "true" : "false"; break;
                case Json::Type::Number: {
                    if (std::isfinite(j.n) && j.n == std::floor(j.n) && std::fabs(j.n) < 1e15) {
                        out += std::to_string(static_cast<long long>(j.n));
                    }
                    else out += "0";
                } break;
                case Json::Type::String: dumpString(out, j.s); break;
                case Json::Type::Array: {
                    out.push_back('[');
                    for (size_t i = 0; i < j.a.size(); i++) {
                        if (i) out.push_back(',');
                        dump(out, j.a[i]);
                    }
                    out.push_back(']');
                } break;
                case Json::Type::Object: {
                    out.push_back('{');
                    for (size_t i = 0; i < j.o.size(); i++) {
                        if (i) out.push_back(',');
                        dumpString(out, j.o[i].first);
                        out.push_back(':');
                        dump(out, j.o[i].second);
                    }
                    out.push_back('}');
                } break;
            }
        }

        std::string line(Json const& j) {
            std::string out;
            dump(out, j);
            out.push_back('\n');
            return out;
        }

        Json msg(char const* type) {
            Json j = Json::object();
            j.set("t", Json::str(type));
            return j;
        }

        // ------------------------------------------------------------------
        // small helpers
        // ------------------------------------------------------------------

        double nowSeconds() {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        }

        int clampInt(Json const* v, int lo, int hi, int fallback) {
            if (!v || v->type != Json::Type::Number || !std::isfinite(v->n)) return fallback;
            double t = std::trunc(v->n);
            return static_cast<int>(std::min<double>(hi, std::max<double>(lo, t)));
        }

        // control characters out, trimmed, at most 20 characters (UTF-8 aware)
        std::string cleanName(std::string raw) {
            std::string s;
            for (unsigned char c : raw) {
                if (c >= 0x20 && c != 0x7F) s.push_back(static_cast<char>(c));
            }
            size_t a = 0, b = s.size();
            while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
            while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
            s = s.substr(a, b - a);

            std::string out;
            int chars = 0;
            for (size_t i = 0; i < s.size(); i++) {
                unsigned char c = static_cast<unsigned char>(s[i]);
                if ((c & 0xC0) != 0x80) {
                    if (chars == 20) break;
                    chars++;
                }
                out.push_back(static_cast<char>(c));
            }
            return out.empty() ? "Player" : out;
        }

        bool validCode(std::string const& code) {
            if (code.size() < 3 || code.size() > 32) return false;
            for (char c : code) {
                bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
                if (!ok) return false;
            }
            return true;
        }

        // the level of the party; keeps the order in which objects were added
        class Store {
        public:
            void set(std::string const& id, std::string const& str) {
                auto it = m_index.find(id);
                if (it != m_index.end()) {
                    m_items[it->second].str = str;
                    return;
                }
                m_index[id] = m_items.size();
                m_items.push_back({ id, str, true });
                m_live++;
            }
            void erase(std::string const& id) {
                auto it = m_index.find(id);
                if (it == m_index.end()) return;
                m_items[it->second].alive = false;
                m_items[it->second].str.clear();
                m_index.erase(it);
                m_live--;
                if (m_items.size() > 1024 && m_items.size() > m_live * 2) compact();
            }
            size_t size() const { return m_live; }

            template <class F>
            void each(F&& f) const {
                for (auto const& item : m_items) {
                    if (item.alive) f(item.id, item.str);
                }
            }

        private:
            struct Item {
                std::string id;
                std::string str;
                bool alive;
            };
            void compact() {
                std::vector<Item> kept;
                kept.reserve(m_live);
                m_index.clear();
                for (auto& item : m_items) {
                    if (!item.alive) continue;
                    m_index[item.id] = kept.size();
                    kept.push_back(std::move(item));
                }
                m_items = std::move(kept);
            }

            std::vector<Item> m_items;
            std::unordered_map<std::string, size_t> m_index;
            size_t m_live = 0;
        };

        struct Client {
            enum class State { New, AwaitingReady, Live };

            sock_t sock = kInvalidSock;
            bool loopback = false;
            bool dead = false;
            bool closeWhenFlushed = false;
            bool isHost = false;
            bool inRoom = false;
            State state = State::New;

            int id = 0;
            std::string name = "Player";
            int icon = 1;
            int c1 = 0;
            int c2 = 3;
            bool glow = false;
            std::vector<std::string> sel;
            int selCount = 0;

            std::string rbuf;
            std::string wbuf;
            size_t woff = 0;
            double lastActive = 0.0;
        };

    }

    // ----------------------------------------------------------------------
    // the server itself
    // ----------------------------------------------------------------------

    struct LocalServer::State {
        sock_t listener = kInvalidSock;
        std::vector<std::unique_ptr<Client>> clients;

        // the one party
        bool created = false;
        bool loading = false;
        bool over = false; // the host is gone
        std::string code;
        int nextId = 0;
        Store objects;

        size_t roomSize() const {
            size_t n = 0;
            for (auto const& c : clients) {
                if (c->inRoom && !c->dead) n++;
            }
            return n;
        }

        // ---- sending ----
        void flush(Client& c) {
            while (c.woff < c.wbuf.size() && !c.dead) {
                int n = static_cast<int>(::send(
                    c.sock, c.wbuf.data() + c.woff,
                    static_cast<int>(std::min<size_t>(c.wbuf.size() - c.woff, 1u << 20)), kSendFlags
                ));
                if (n > 0) {
                    c.woff += static_cast<size_t>(n);
                }
                else if (n < 0 && wouldBlock()) {
                    break;
                }
                else {
                    c.dead = true;
                    return;
                }
            }
            if (c.woff >= c.wbuf.size()) {
                c.wbuf.clear();
                c.woff = 0;
                if (c.closeWhenFlushed) c.dead = true;
            }
            else if (c.woff > (1u << 20) && c.woff * 2 > c.wbuf.size()) {
                c.wbuf.erase(0, c.woff);
                c.woff = 0;
            }
        }

        void sendRaw(Client& c, std::string const& text) {
            if (c.dead) return;
            c.wbuf += text;
            if (c.wbuf.size() - c.woff > kMaxPending) {
                c.dead = true;
                return;
            }
            this->flush(c);
        }

        void send(Client& c, Json const& j) {
            this->sendRaw(c, line(j));
        }

        void fail(Client& c, std::string const& text) {
            Json j = msg("error");
            j.set("msg", Json::str(text));
            this->send(c, j);
            c.closeWhenFlushed = true;
            this->flush(c);
        }

        // ---- the party ----
        void broadcastPlayers() {
            Json list = Json::array();
            for (auto const& c : clients) {
                if (!c->inRoom || c->dead) continue;
                Json p = Json::object();
                p.set("id", Json::num(c->id));
                p.set("name", Json::str(c->name));
                p.set("icon", Json::num(c->icon));
                p.set("c1", Json::num(c->c1));
                p.set("c2", Json::num(c->c2));
                p.set("glow", Json::boolean(c->glow));
                list.add(std::move(p));
            }
            Json out = msg("players");
            out.set("list", std::move(list));
            std::string text = line(out);
            for (auto const& c : clients) {
                if (c->inRoom && !c->dead) this->sendRaw(*c, text);
            }
        }

        void leave(Client& c) {
            if (!c.inRoom) return;
            c.inRoom = false;
            if (c.isHost) {
                // no host, no party
                over = true;
                for (auto const& other : clients) {
                    if (other.get() != &c && other->inRoom && !other->dead) {
                        this->fail(*other, "The host left, the party is over");
                    }
                }
                return;
            }
            if (!over && roomSize() > 0) this->broadcastPlayers();
        }

        void onHello(Client& c, Json const& m) {
            if (c.inRoom) return;
            Json const* ver = m.get("ver");
            if (!ver || ver->type != Json::Type::Number || static_cast<int>(ver->n) != kProtocolVersion) {
                return this->fail(c, "Version mismatch: update the mod and the server to the same version");
            }
            std::string const mode = m.getStr("mode");
            if (mode == "lobby") {
                return this->fail(c, "This party has no invite service, ask for the party code instead");
            }

            std::string code = m.getStr("room");
            for (auto& ch : code) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (!validCode(code)) {
                return this->fail(c, "Invalid room code (use 3-32 letters or digits)");
            }
            bool const host = mode == "host";

            if (host) {
                // only the player on this very computer may create the party
                if (!c.loopback) return this->fail(c, "Only the host can start the party");
                if (created) return this->fail(c, "That room code is already in use - pick another one, or press Join");
                created = true;
                loading = true;
                this->code = code;
            }
            else {
                if (!created || over || code != this->code) return this->fail(c, "Room not found - check the code");
                if (loading) return this->fail(c, "The room is still being created, try again in a few seconds");
            }
            if (roomSize() >= kMaxClients) return this->fail(c, "The room is full");

            c.id = ++nextId;
            c.name = cleanName(m.getStr("name", "Player"));
            c.icon = clampInt(m.get("icon"), 1, 484, 1);
            c.c1 = clampInt(m.get("c1"), 0, 255, 0);
            c.c2 = clampInt(m.get("c2"), 0, 255, 3);
            auto glow = m.get("glow");
            c.glow = glow && glow->type == Json::Type::Bool && glow->b;
            c.inRoom = true;
            c.isHost = host;
            c.state = host ? Client::State::Live : Client::State::AwaitingReady;

            Json welcome = msg("welcome");
            welcome.set("id", Json::num(c.id));
            welcome.set("room", Json::str(this->code));
            welcome.set("empty", Json::boolean(host));
            welcome.set("count", Json::num(static_cast<double>(objects.size())));
            this->send(c, welcome);
            this->broadcastPlayers();
        }

        void onReady(Client& c) {
            if (!c.inRoom || c.state != Client::State::AwaitingReady) return;

            Json chunk = Json::array();
            auto flushChunk = [&] {
                if (chunk.a.empty()) return;
                Json out = msg("snapshot");
                out.set("objs", std::move(chunk));
                this->send(c, out);
                chunk = Json::array();
            };
            objects.each([&](std::string const& id, std::string const& str) {
                Json entry = Json::array();
                entry.add(Json::str(id));
                entry.add(Json::str(str));
                chunk.add(std::move(entry));
                if (chunk.a.size() >= kSnapshotChunk) flushChunk();
            });
            flushChunk();
            this->send(c, msg("snapshot_end"));
            c.state = Client::State::Live;

            // show the newcomer what everybody else has selected right now
            for (auto const& other : clients) {
                if (other.get() != &c && other->inRoom && other->state == Client::State::Live && !other->sel.empty()) {
                    this->send(c, selMessage(*other));
                }
            }
        }

        static Json selMessage(Client const& from) {
            Json ids = Json::array();
            for (auto const& id : from.sel) ids.add(Json::str(id));
            Json out = msg("sel");
            out.set("from", Json::num(from.id));
            out.set("ids", std::move(ids));
            out.set("n", Json::num(from.selCount));
            return out;
        }

        void onSnapshot(Client& c, Json const& m) {
            auto objs = m.get("objs");
            if (!c.inRoom || !loading || !objs || !objs->isArray()) return;
            for (auto const& entry : objs->a) {
                if (!entry.isArray() || entry.a.size() < 2) continue;
                auto const& id = entry.a[0];
                auto const& str = entry.a[1];
                if (id.isString() && str.isString() && id.s.size() < 64 && str.s.size() <= kMaxObjectString) {
                    objects.set(id.s, str.s);
                }
            }
        }

        void onSnapshotEnd(Client& c) {
            if (!c.inRoom || !loading) return;
            loading = false;
        }

        void onOps(Client& c, Json const& m) {
            auto ops = m.get("ops");
            if (!c.inRoom || loading || c.state != Client::State::Live || !ops || !ops->isArray()) return;

            Json accepted = Json::array();
            for (auto const& op : ops->a) {
                if (!op.isArray() || op.a.size() < 2 || !op.a[0].isString() || !op.a[1].isString()) continue;
                std::string const& kind = op.a[0].s;
                std::string const& id = op.a[1].s;
                if (id.empty() || id.size() >= 64) continue;

                if (kind == "d") {
                    objects.erase(id);
                    Json out = Json::array();
                    out.add(Json::str("d"));
                    out.add(Json::str(id));
                    accepted.add(std::move(out));
                }
                else if ((kind == "a" || kind == "u") && op.a.size() >= 3 && op.a[2].isString()
                         && op.a[2].s.size() <= kMaxObjectString) {
                    objects.set(id, op.a[2].s);
                    Json out = Json::array();
                    out.add(Json::str(kind));
                    out.add(Json::str(id));
                    out.add(Json::str(op.a[2].s));
                    accepted.add(std::move(out));
                }
            }
            if (accepted.a.empty()) return;

            Json out = msg("ops");
            out.set("from", Json::num(c.id));
            out.set("ops", std::move(accepted));
            std::string text = line(out);
            for (auto const& other : clients) {
                if (other.get() != &c && other->inRoom && other->state == Client::State::Live) {
                    this->sendRaw(*other, text);
                }
            }
        }

        void onSel(Client& c, Json const& m) {
            auto ids = m.get("ids");
            if (!c.inRoom || loading || c.state != Client::State::Live || !ids || !ids->isArray()) return;

            c.sel.clear();
            for (auto const& id : ids->a) {
                if (id.isString() && !id.s.empty() && id.s.size() < 64) c.sel.push_back(id.s);
                if (c.sel.size() >= kMaxSelection) break;
            }
            c.selCount = std::max<int>(static_cast<int>(c.sel.size()), clampInt(m.get("n"), 0, 100000000, static_cast<int>(c.sel.size())));

            std::string text = line(selMessage(c));
            for (auto const& other : clients) {
                if (other.get() != &c && other->inRoom && other->state == Client::State::Live) {
                    this->sendRaw(*other, text);
                }
            }
        }

        void onInvite(Client& c, Json const& m) {
            if (!c.inRoom || loading || c.state != Client::State::Live) return;
            Json out = msg("invite_result");
            out.set("to", Json::str(m.getStr("to")));
            out.set("sent", Json::num(0));
            out.set("reason", Json::str("no_server"));
            this->send(c, out);
        }

        void onLine(Client& c, std::string_view text) {
            Json m;
            if (!Parser(text).parse(m) || !m.isObject()) return;
            std::string const t = m.getStr("t");
            if (t == "hello") onHello(c, m);
            else if (t == "ready") onReady(c);
            else if (t == "snapshot") onSnapshot(c, m);
            else if (t == "snapshot_end") onSnapshotEnd(c);
            else if (t == "ops") onOps(c, m);
            else if (t == "sel") onSel(c, m);
            else if (t == "invite") onInvite(c, m);
            else if (t == "ping") this->send(c, msg("pong"));
        }

        void onData(Client& c, char const* data, size_t size) {
            c.rbuf.append(data, size);
            if (c.rbuf.size() > kMaxLineBytes) {
                return this->fail(c, "Message too large");
            }
            size_t start = 0;
            while (true) {
                size_t nl = c.rbuf.find('\n', start);
                if (nl == std::string::npos) break;
                std::string_view view(c.rbuf.data() + start, nl - start);
                while (!view.empty() && (view.back() == '\r' || view.back() == ' ')) view.remove_suffix(1);
                while (!view.empty() && (view.front() == ' ')) view.remove_prefix(1);
                if (!view.empty()) this->onLine(c, view);
                start = nl + 1;
                if (c.dead || c.closeWhenFlushed) break;
            }
            c.rbuf.erase(0, start);
        }
    };

    LocalServer::LocalServer() : m_state(std::make_unique<State>()) {}

    LocalServer::~LocalServer() {
        this->stop();
    }

    int LocalServer::start(int firstPort, std::string& error) {
        if (m_running) return m_port;
        ensureSocketsStarted();

        sock_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock == kInvalidSock) {
            error = "Could not create a network socket";
            return 0;
        }

        int bound = 0;
        for (int port = firstPort; port < firstPort + 10; port++) {
            sockaddr_in addr {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons(static_cast<unsigned short>(port));
#ifndef _WIN32
            int yes = 1;
            ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char const*>(&yes), sizeof(yes));
#endif
            if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                bound = port;
                break;
            }
        }
        if (bound == 0 || ::listen(sock, 16) != 0) {
            closeSock(sock);
            error = "Could not open a port for the party (all of 5588-5597 are in use?)";
            return 0;
        }
        setNonBlocking(sock);

        *m_state = State {};
        m_state->listener = sock;
        m_port = bound;
        m_stop = false;
        m_running = true;
        m_thread = std::thread([this] { this->run(); });
        return m_port;
    }

    void LocalServer::stop() {
        if (!m_running && !m_thread.joinable()) return;
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
        m_running = false;
    }

    void LocalServer::run() {
        State& st = *m_state;

        while (!m_stop) {
            fd_set rd, wr;
            FD_ZERO(&rd);
            FD_ZERO(&wr);
            FD_SET(st.listener, &rd);
            sock_t maxFd = st.listener;
            for (auto const& c : st.clients) {
                FD_SET(c->sock, &rd);
                if (c->woff < c->wbuf.size()) FD_SET(c->sock, &wr);
                maxFd = std::max(maxFd, c->sock);
            }

            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 30 * 1000;
#ifdef _WIN32
            int ready = ::select(0, &rd, &wr, nullptr, &tv);
#else
            int ready = ::select(static_cast<int>(maxFd) + 1, &rd, &wr, nullptr, &tv);
#endif
            double const now = nowSeconds();

            if (ready > 0) {
                // new connections
                if (FD_ISSET(st.listener, &rd)) {
                    while (true) {
                        sockaddr_in peer {};
                        sock_len_t len = sizeof(peer);
                        sock_t s = ::accept(st.listener, reinterpret_cast<sockaddr*>(&peer), &len);
                        if (s == kInvalidSock) break;
                        if (st.clients.size() >= kMaxConnections) {
                            closeSock(s);
                            continue;
                        }
                        setNonBlocking(s);
                        int one = 1;
                        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&one), sizeof(one));

                        auto client = std::make_unique<Client>();
                        client->sock = s;
                        client->loopback = (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
                        client->lastActive = now;
                        st.clients.push_back(std::move(client));
                    }
                }

                for (size_t i = 0; i < st.clients.size(); i++) {
                    Client& c = *st.clients[i];
                    if (c.dead) continue;

                    if (FD_ISSET(c.sock, &wr)) st.flush(c);

                    if (FD_ISSET(c.sock, &rd) && !c.closeWhenFlushed) {
                        static thread_local std::vector<char> buffer(256 * 1024);
                        int n = static_cast<int>(::recv(c.sock, buffer.data(), static_cast<int>(buffer.size()), 0));
                        if (n > 0) {
                            c.lastActive = now;
                            st.onData(c, buffer.data(), static_cast<size_t>(n));
                        }
                        else if (n == 0 || !wouldBlock()) {
                            c.dead = true;
                        }
                    }
                }
            }

            // silent connections
            for (auto const& c : st.clients) {
                if (!c->dead && !c->closeWhenFlushed && now - c->lastActive > kIdleSeconds) {
                    st.fail(*c, "Timed out");
                }
            }

            // forget the dead (leaving the party can kick other people, so repeat)
            for (bool again = true; again;) {
                again = false;
                for (size_t i = 0; i < st.clients.size(); i++) {
                    if (!st.clients[i]->dead) continue;
                    st.leave(*st.clients[i]);
                    closeSock(st.clients[i]->sock);
                    st.clients.erase(st.clients.begin() + static_cast<long>(i));
                    again = true;
                    break;
                }
            }
        }

        // goodbye: tell everybody once, then close everything
        for (auto const& c : st.clients) {
            if (c->inRoom && !c->isHost && !c->dead) {
                Json j = msg("error");
                j.set("msg", Json::str("The host closed the party"));
                st.send(*c, j);
            }
        }
        double const until = nowSeconds() + .3;
        while (nowSeconds() < until) {
            bool pending = false;
            for (auto const& c : st.clients) {
                if (!c->dead && c->woff < c->wbuf.size()) {
                    st.flush(*c);
                    pending = pending || (!c->dead && c->woff < c->wbuf.size());
                }
            }
            if (!pending) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        for (auto const& c : st.clients) closeSock(c->sock);
        st.clients.clear();
        closeSock(st.listener);
        st.listener = kInvalidSock;
    }

}
