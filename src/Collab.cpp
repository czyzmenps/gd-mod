#include "Collab.hpp"
#include "Config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <random>
#include <unordered_set>

using namespace geode::prelude;

namespace collab {

    namespace {
        constexpr int kProtocolVersion = 1;
        constexpr size_t kSnapshotChunk = 250;
        constexpr size_t kOpsPerMessage = 200;

        std::string getStr(matjson::Value const& v, char const* key, std::string def = "") {
            if (!v.isObject() || !v.contains(key)) return def;
            return v[key].asString().unwrapOr(def);
        }

        int64_t getInt(matjson::Value const& v, char const* key, int64_t def = 0) {
            if (!v.isObject() || !v.contains(key)) return def;
            return static_cast<int64_t>(v[key].asInt().unwrapOr(static_cast<std::intmax_t>(def)));
        }

        bool getBool(matjson::Value const& v, char const* key, bool def = false) {
            if (!v.isObject() || !v.contains(key)) return def;
            return v[key].asBool().unwrapOr(def);
        }

        matjson::Value const& at(matjson::Value const& v, size_t index) {
            return v[index];
        }

        std::string trim(std::string s) {
            auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
            s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
            return s;
        }

        // "tcp://example.com:5588/" -> address "example.com", port 5588 (default 5588)
        bool parseServer(std::string server, std::string& address, int& port, std::string& error) {
            server = trim(server);
            if (auto pos = server.find("://"); pos != std::string::npos) {
                server = server.substr(pos + 3);
            }
            while (!server.empty() && server.back() == '/') server.pop_back();

            port = 5588;
            if (auto colon = server.rfind(':'); colon != std::string::npos && server.find(':') == colon) {
                try {
                    port = std::stoi(server.substr(colon + 1));
                }
                catch (...) {
                    error = "The port in the server address is not a number";
                    return false;
                }
                server = server.substr(0, colon);
            }
            if (server.empty()) {
                error = "The party code is incomplete - paste the whole code (it contains @)";
                return false;
            }
            if (port < 1 || port > 65535) {
                error = "The port must be between 1 and 65535";
                return false;
            }
            address = server;
            return true;
        }

        std::string normalizeServer(std::string const& server) {
            std::string address, error;
            int port = 5588;
            if (!parseServer(server, address, port, error)) return "";
            return fmt::format("{}:{}", address, port);
        }

        // luminance 0..255, used to avoid outlines that vanish on the dark editor background
        int brightness(ccColor3B c) {
            return (c.r * 299 + c.g * 587 + c.b * 114) / 1000;
        }

        // GD tag characters would break the text of a popup
        std::string plain(std::string s) {
            for (auto& c : s) {
                if (c == '<' || c == '>') c = '?';
            }
            return s;
        }
    }

    Manager& Manager::get() {
        static Manager instance;
        return instance;
    }

    double Manager::msSince(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    std::string Manager::makeRoomCode() {
        static constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> dist(0, sizeof(alphabet) - 2);
        std::string code;
        for (int i = 0; i < 8; i++) code.push_back(alphabet[dist(gen)]);
        return code;
    }

    // ------------------------------------------------------------------
    // editor lifetime
    // ------------------------------------------------------------------

    void Manager::attach(EditorUI* ui, LevelEditorLayer* layer) {
        if (m_ui) this->detach();
        m_ui = ui;
        m_lel = layer;
        m_status = Status::Offline;
        m_statusText = "Not connected";

        this->refreshInviteSettings();
    }

    void Manager::detach() {
        m_net.close();
        this->stopLocalParty();
        m_lobbyWanted = false;
        this->stopLobby();
        this->clearSelections();
        m_overlay.swap(nullptr);
        m_remote.clear();
        m_out.clear();
        m_toClear.clear();
        m_uploadList.clear();
        m_uploading = false;
        m_players.clear();
        this->clearTracking();
        m_hud.swap(nullptr);
        m_badge.swap(nullptr);
        m_launcher.swap(nullptr);
        m_ui = nullptr;
        m_lel = nullptr;
        m_status = Status::Offline;
        m_statusText = "Not connected";
    }

    void Manager::setOverlay(SelectionOverlay* overlay) {
        m_overlay.swap(overlay);
        m_overlayShown = false;
    }

    void Manager::setLauncher(CollabLauncher* launcher) {
        m_launcher.swap(launcher);
        if (m_launcher) m_launcher->applySettings();
    }

    void Manager::onSettingChanged(std::string_view key) {
        if (!m_ui) return;
        if (key == "server" || key == "player-name" || key == "receive-invites") {
            this->refreshInviteSettings();
        }
        else if (key == "button-position" || key == "button-size") {
            if (m_launcher) m_launcher->applySettings();
        }
    }

    std::string Manager::partyCode() const {
        // not known yet while the router is still being asked to open the port
        if (m_room.empty() || m_localStage != LocalStage::None) return "";
        std::string const defaultServer = normalizeServer(std::string(cfg::kDefaultServer));
        if (m_serverLabel.empty() || (!defaultServer.empty() && m_serverLabel == defaultServer)) {
            return m_room;
        }
        return m_room + "@" + m_serverLabel;
    }

    void Manager::setHud(CollabHud* hud, CollabDot* badge) {
        m_hud.swap(hud);
        m_badge.swap(badge);
        this->updateHud();
    }

    void Manager::setStatus(Status status, std::string text) {
        m_status = status;
        m_statusText = std::move(text);
        this->updateHud();
    }

    float Manager::progress() const {
        if (m_status != Status::Syncing) return -1.f;
        if (m_uploading) {
            if (m_uploadList.empty()) return -1.f;
            return std::min(1.f, static_cast<float>(m_uploadPos) / static_cast<float>(m_uploadList.size()));
        }
        if (!m_toClear.empty() || m_downloadTotal == 0) return -1.f;
        return std::min(1.f, static_cast<float>(m_downloaded) / static_cast<float>(m_downloadTotal));
    }

    std::string Manager::hudText() const {
        switch (m_status) {
            case Status::Offline: return "";
            case Status::Live:
                return fmt::format("{}  -  {} online", m_room, std::max<size_t>(1, m_players.size()));
            case Status::Syncing: {
                float const p = this->progress();
                if (p < 0.f) return fmt::format("{}  -  syncing...", m_room);
                return fmt::format("{}  -  syncing {}%", m_room, static_cast<int>(p * 100.f));
            }
            default: return m_statusText;
        }
    }

    void Manager::updateHud() {
        if (m_hud) m_hud->setState(m_status, this->hudText(), this->progress());
        if (m_badge) m_badge->setStatus(m_status);
    }

    // ------------------------------------------------------------------
    // connection management
    // ------------------------------------------------------------------

    // Forgets everything about the previous party and sets up a new one.
    void Manager::beginSession(bool host, std::string room, std::string name) {
        this->stopLobby();

        m_remote.clear();
        m_out.clear();
        m_toClear.clear();
        m_uploadList.clear();
        m_uploading = false;
        m_snapshotDone = false;
        m_players.clear();
        this->clearTracking();
        this->clearSelections();

        m_hosting = host;
        m_room = std::move(room);
        m_name = std::move(name);
        m_serverLabel.clear();
        m_clientId = 0;
        m_counter = 0;
        m_pingTimer = 0.f;
        m_scanTimer = 0.f;
        m_reconcileTimer = 0.f;
        m_sweepPos = 0;
        m_selCursor = 0;
        m_prevSelection.clear();
    }

    void Manager::connect(std::string server, std::string room, std::string name, bool host) {
        if (!m_ui || m_status != Status::Offline) return;

        m_hint.clear();
        room = trim(room);
        name = trim(name);
        if (name.empty()) name = "Player";
        if (name.size() > 20) name.resize(20);

        // no server: this game hosts the party itself
        if (host && trim(server).empty()) {
            this->startLocalParty(std::move(room), std::move(name));
            return;
        }

        std::string address;
        int port = 5588;
        {
            std::string error;
            if (!parseServer(server, address, port, error)) {
                this->setStatus(Status::Offline, error);
                return;
            }
        }
        server = address;
        if (!host && room.empty()) {
            this->setStatus(Status::Offline, "Enter the room code you want to join");
            return;
        }
        if (host && room.empty()) room = makeRoomCode();
        std::transform(room.begin(), room.end(), room.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });

        this->beginSession(host, std::move(room), std::move(name));
        m_serverLabel = fmt::format("{}:{}", server, port);

        this->setStatus(Status::Connecting, fmt::format("Connecting to {}:{}...", server, port));
        m_net.connect(server, port);
    }

    // ------------------------------------------------------------------
    // a party hosted by this game
    // ------------------------------------------------------------------

    void Manager::startLocalParty(std::string room, std::string name) {
        if (room.empty()) room = makeRoomCode();
        std::transform(room.begin(), room.end(), room.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });

        std::string error;
        int const port = m_local.start(5588, error);
        if (port == 0) {
            this->setStatus(Status::Offline, "Could not start the party");
            m_hint = error;
            return;
        }

        this->beginSession(true, std::move(room), std::move(name));
        m_localActive = true;
        m_localPort = port;

        // the address friends use, typed by the player (tunnel, manual port forwarding) ...
        std::string manual = cfg::text("public-address");
        if (auto pos = manual.find("://"); pos != std::string::npos) manual = manual.substr(pos + 3);
        while (!manual.empty() && manual.back() == '/') manual.pop_back();
        if (!manual.empty()) {
            m_localAddress = manual.find(':') == std::string::npos ? fmt::format("{}:{}", manual, port) : manual;
            this->connectLocal();
            return;
        }

        // ... or found out by asking the router to open the port
        m_mapper.open(port);
        m_localStage = LocalStage::OpeningPort;
        this->setStatus(Status::Connecting, fmt::format("Opening port {} in your router...", port));
    }

    void Manager::tickLocalParty() {
        if (m_localStage != LocalStage::OpeningPort || m_mapper.working()) return;

        UpnpResult const result = m_mapper.result();
        if (!result.ok) {
            std::string const hint = result.hint;
            this->disconnect(result.error);
            m_hint = hint;
            return;
        }
        m_localAddress = fmt::format("{}:{}", result.address, m_localPort);
        this->connectLocal();
    }

    // the host's own game connects to the server inside it
    void Manager::connectLocal() {
        m_localStage = LocalStage::None;
        m_serverLabel = m_localAddress;
        this->setStatus(Status::Connecting, "Starting the party...");
        m_net.connect("127.0.0.1", m_localPort);
    }

    void Manager::stopLocalParty() {
        m_localStage = LocalStage::None;
        m_localActive = false;
        m_mapper.close();
        m_local.stop();
    }

    void Manager::disconnect(std::string const& reason) {
        m_net.close();
        this->stopLocalParty();
        m_remote.clear();
        m_out.clear();
        m_toClear.clear();
        m_uploadList.clear();
        m_uploading = false;
        m_players.clear();
        m_skipConfirm = false;
        this->clearTracking();
        this->clearSelections();
        this->setStatus(Status::Offline, reason);
        m_lobbyRetry = 1.f; // come back to the invite lobby shortly
    }

    void Manager::sendJson(matjson::Value const& msg) {
        m_net.send(msg.dump(matjson::NO_INDENTATION));
    }

    void Manager::handleEvent(NetEvent const& ev) {
        switch (ev.type) {
            case NetEvent::Type::Connected: {
                this->setStatus(Status::Joining, "Connected, entering the room...");
                auto* gm = GameManager::get();
                this->sendJson(matjson::makeObject({
                    {"t", "hello"},
                    {"ver", kProtocolVersion},
                    {"mode", m_hosting ? "host" : "join"},
                    {"room", m_room},
                    {"name", m_name},
                    {"icon", gm->getPlayerFrame()},
                    {"c1", gm->getPlayerColor()},
                    {"c2", gm->getPlayerColor2()},
                    {"glow", gm->getPlayerGlow()},
                }));
            } break;
            case NetEvent::Type::Line: {
                this->handleMessage(ev.text);
            } break;
            case NetEvent::Type::Closed: {
                if (m_status != Status::Offline) this->disconnect(ev.text);
            } break;
        }
    }

    void Manager::handleMessage(std::string const& line) {
        auto parsed = matjson::Value::parse(line);
        if (parsed.isErr()) return;
        matjson::Value const& msg = parsed.unwrap();
        std::string const type = getStr(msg, "t");

        if (type == "welcome") {
            m_clientId = static_cast<int>(getInt(msg, "id"));
            m_room = getStr(msg, "room", m_room);
            m_counter = 0;
            if (getBool(msg, "empty")) {
                if (m_hosting && m_ui && cfg::flag("copy-code-on-start")) {
                    geode::utils::clipboard::write(this->partyCode());
                    std::string const text = fmt::format("Party {} started - code copied", m_room);
                    Notification::create(text.c_str(), NotificationIcon::Success, 3.f)->show();
                }
                this->beginUpload();
            }
            else {
                m_downloadTotal = static_cast<size_t>(getInt(msg, "count"));
                this->askToReplaceLevel(static_cast<int>(m_downloadTotal));
            }
        }
        else if (type == "snapshot") {
            if (m_status != Status::Syncing) return;
            if (!msg.contains("objs") || !msg["objs"].isArray()) return;
            auto const& objs = msg["objs"];
            for (size_t i = 0; i < objs.size(); i++) {
                auto const& item = at(objs, i);
                if (!item.isArray() || item.size() < 2) continue;
                m_remote.push_back(RemoteOp {
                    RemoteOp::Kind::Add,
                    at(item, 0).asString().unwrapOr(""),
                    at(item, 1).asString().unwrapOr(""),
                });
            }
        }
        else if (type == "snapshot_end") {
            m_snapshotDone = true;
        }
        else if (type == "ops") {
            if (m_status != Status::Syncing && m_status != Status::Live) return;
            if (!msg.contains("ops") || !msg["ops"].isArray()) return;
            auto const& ops = msg["ops"];
            for (size_t i = 0; i < ops.size(); i++) {
                auto const& item = at(ops, i);
                if (!item.isArray() || item.size() < 2) continue;
                std::string const kind = at(item, 0).asString().unwrapOr("");
                std::string const id = at(item, 1).asString().unwrapOr("");
                if (id.empty()) continue;
                if (kind == "d") {
                    m_remote.push_back(RemoteOp { RemoteOp::Kind::Remove, id, "" });
                }
                else if ((kind == "a" || kind == "u") && item.size() >= 3) {
                    m_remote.push_back(RemoteOp {
                        kind == "a" ? RemoteOp::Kind::Add : RemoteOp::Kind::Update,
                        id,
                        at(item, 2).asString().unwrapOr(""),
                    });
                }
            }
        }
        else if (type == "players") {
            std::vector<PlayerInfo> before = std::move(m_players);
            m_players.clear();
            if (msg.contains("list") && msg["list"].isArray()) {
                auto const& list = msg["list"];
                for (size_t i = 0; i < list.size(); i++) {
                    auto const& p = at(list, i);
                    PlayerInfo info;
                    info.id = static_cast<int>(getInt(p, "id"));
                    info.name = getStr(p, "name", "?");
                    info.icon = static_cast<int>(std::clamp<int64_t>(getInt(p, "icon", 1), 1, 484));
                    info.c1 = static_cast<int>(std::clamp<int64_t>(getInt(p, "c1", 0), 0, 255));
                    info.c2 = static_cast<int>(std::clamp<int64_t>(getInt(p, "c2", 3), 0, 255));
                    info.glow = getBool(p, "glow", false);
                    m_players.push_back(std::move(info));
                }
            }
            // selections of players that are gone
            for (auto it = m_remoteSel.begin(); it != m_remoteSel.end();) {
                int const id = it->first;
                bool const present = std::any_of(m_players.begin(), m_players.end(), [id](PlayerInfo const& p) {
                    return p.id == id;
                });
                it = present ? std::next(it) : m_remoteSel.erase(it);
            }
            this->announcePlayers(before);
            this->updateHud();
        }
        else if (type == "sel") {
            if (m_status != Status::Syncing && m_status != Status::Live) return;
            int const from = static_cast<int>(getInt(msg, "from"));
            if (from <= 0 || from == m_clientId) return;

            RemoteSelection sel;
            if (msg.contains("ids") && msg["ids"].isArray()) {
                auto const& ids = msg["ids"];
                for (size_t i = 0; i < ids.size() && i < 300; i++) {
                    std::string id = at(ids, i).asString().unwrapOr("");
                    if (!id.empty()) sel.ids.push_back(std::move(id));
                }
            }
            sel.count = std::max<size_t>(sel.ids.size(), static_cast<size_t>(std::max<int64_t>(0, getInt(msg, "n"))));
            if (sel.count == 0) m_remoteSel.erase(from);
            else m_remoteSel[from] = std::move(sel);
        }
        else if (type == "invite_result") {
            if (!m_ui) return;
            std::string const to = plain(getStr(msg, "to"));
            if (getInt(msg, "sent") > 0) {
                std::string const text = fmt::format("Invite sent to {}", to);
                Notification::create(text.c_str(), NotificationIcon::Success, 2.f)->show();
            }
            else if (getStr(msg, "reason") == "slow_down") {
                Notification::create("Wait a moment before sending another invite", NotificationIcon::Warning, 2.f)->show();
            }
            else if (getStr(msg, "reason") == "no_server") {
                Notification::create(
                    "Invites need a party server. Send your friends the party code instead.", NotificationIcon::Warning, 4.f
                )->show();
            }
            else {
                std::string const text = fmt::format(
                    "{} is not waiting for invites. They need to be on this server with \"Receive invites\" switched on.", to
                );
                Notification::create(text.c_str(), NotificationIcon::Warning, 4.f)->show();
            }
        }
        else if (type == "error") {
            this->disconnect(getStr(msg, "msg", "The server refused the connection"));
        }
        // "pong" and unknown messages are ignored
    }

    // Small toasts when somebody enters or leaves the room.
    void Manager::announcePlayers(std::vector<PlayerInfo> const& before) {
        if (m_status != Status::Live || !m_ui || !cfg::flag("join-notifications")) return;

        auto contains = [](std::vector<PlayerInfo> const& list, int id) {
            return std::any_of(list.begin(), list.end(), [id](PlayerInfo const& p) { return p.id == id; });
        };

        for (auto const& p : m_players) {
            if (p.id == m_clientId || contains(before, p.id)) continue;
            std::string const text = fmt::format("{} joined the room", p.name);
            Notification::create(text.c_str(), NotificationIcon::Success, 2.f)->show();
        }
        for (auto const& p : before) {
            if (p.id == m_clientId || contains(m_players, p.id)) continue;
            std::string const text = fmt::format("{} left the room", p.name);
            Notification::create(text.c_str(), NotificationIcon::Info, 2.f)->show();
        }
    }

    void Manager::askToReplaceLevel(int objectCount) {
        if (m_skipConfirm) {
            // the invitation window already asked
            m_skipConfirm = false;
            this->beginDownload();
            return;
        }
        this->setStatus(Status::Confirm, "Waiting for your confirmation...");
        createQuickPopup(
            "Join room",
            fmt::format(
                "Room <cy>{}</c> contains <cg>{}</c> objects.\n\n"
                "Joining <cr>deletes everything in this editor</c> and loads the room's level instead. "
                "Save a copy of your level first if you want to keep it!",
                m_room, objectCount
            ),
            "Cancel", "Join",
            [](FLAlertLayer*, bool join) {
                auto& self = Manager::get();
                if (self.m_status != Status::Confirm || !self.m_ui) return;
                if (!join) {
                    self.disconnect("Join cancelled");
                    return;
                }
                self.beginDownload();
            }
        );
    }

    // ------------------------------------------------------------------
    // initial sync
    // ------------------------------------------------------------------

    void Manager::beginDownload() {
        m_ui->deselectAll();
        this->clearTracking();
        m_remote.clear();
        m_toClear.clear();
        if (auto* arr = m_lel->m_objects) {
            for (unsigned int i = 0; i < arr->count(); i++) {
                m_toClear.emplace_back(static_cast<GameObject*>(arr->objectAtIndex(i)));
            }
        }
        if (m_lel->m_undoObjects) m_lel->m_undoObjects->removeAllObjects();
        if (m_lel->m_redoObjects) m_lel->m_redoObjects->removeAllObjects();
        m_snapshotDone = false;
        m_downloaded = 0;
        this->setStatus(Status::Syncing, "Loading level...");
        this->sendJson(matjson::makeObject({{"t", "ready"}}));
    }

    void Manager::beginUpload() {
        this->clearTracking();
        m_uploadList.clear();
        if (auto* arr = m_lel->m_objects) {
            for (unsigned int i = 0; i < arr->count(); i++) {
                m_uploadList.emplace_back(static_cast<GameObject*>(arr->objectAtIndex(i)));
            }
        }
        m_uploadPos = 0;
        m_uploading = true;
        this->setStatus(Status::Syncing, "Uploading level...");
    }

    void Manager::stepUpload() {
        auto const start = Clock::now();
        auto chunk = matjson::Value::array();
        size_t inChunk = 0;

        auto sendChunk = [&] {
            if (inChunk == 0) return;
            this->sendJson(matjson::makeObject({{"t", "snapshot"}, {"objs", chunk}}));
            chunk = matjson::Value::array();
            inChunk = 0;
        };

        while (m_uploadPos < m_uploadList.size() && msSince(start) < 8.0) {
            GameObject* obj = m_uploadList[m_uploadPos++];
            if (!obj || m_byObj.contains(obj)) continue;
            std::string data = this->saveString(obj);
            std::string id = this->newId();
            this->track(obj, id, std::hash<std::string>{}(data));
            if (data.empty()) continue;

            auto item = matjson::Value::array();
            item.push(id);
            item.push(data);
            chunk.push(std::move(item));
            if (++inChunk >= kSnapshotChunk) sendChunk();
        }
        sendChunk();

        if (m_uploadPos >= m_uploadList.size()) {
            this->sendJson(matjson::makeObject({{"t", "snapshot_end"}}));
            m_uploading = false;
            m_uploadList.clear();
            this->setStatus(Status::Live, fmt::format("Live in room {}", m_room));
        }
        else {
            this->setStatus(
                Status::Syncing,
                fmt::format("Uploading level... {} / {}", m_uploadPos, m_uploadList.size())
            );
        }
    }

    // ------------------------------------------------------------------
    // object bookkeeping
    // ------------------------------------------------------------------

    std::string Manager::saveString(GameObject* obj) const {
        std::string s = static_cast<std::string>(obj->getSaveString(m_lel));
        if (!s.empty() && s.back() != ';') s.push_back(';');
        return s;
    }

    std::string Manager::newId() {
        return fmt::format("{}.{}", m_clientId, ++m_counter);
    }

    void Manager::track(GameObject* obj, std::string id, size_t hash) {
        if (auto it = m_byObj.find(obj); it != m_byObj.end()) {
            m_byId.erase(it->second.id);
        }
        if (auto it = m_byId.find(id); it != m_byId.end() && it->second != obj) {
            // the id is being reassigned to a new object; forget the old one
            m_byObj.erase(it->second);
        }
        ObjInfo info;
        info.id = id;
        info.hash = hash;
        info.keep = obj;
        m_byId[id] = obj;
        m_byObj[obj] = std::move(info);
    }

    void Manager::untrack(GameObject* obj) {
        auto it = m_byObj.find(obj);
        if (it == m_byObj.end()) return;
        if (auto idIt = m_byId.find(it->second.id); idIt != m_byId.end() && idIt->second == obj) {
            m_byId.erase(idIt);
        }
        m_byObj.erase(it);
    }

    void Manager::clearTracking() {
        m_byId.clear();
        m_byObj.clear();
        m_prevSelection.clear();
    }

    GameObject* Manager::findById(std::string const& id) const {
        auto it = m_byId.find(id);
        return it == m_byId.end() ? nullptr : it->second;
    }

    bool Manager::isPlaytesting() const {
        return m_lel && m_lel->m_playbackMode != PlaybackMode::Not;
    }

    // ------------------------------------------------------------------
    // applying remote changes
    // ------------------------------------------------------------------

    void Manager::removeLocal(GameObject* obj) {
        Ref<GameObject> hold = obj; // survive the removal
        if (obj->m_isSelected && m_ui) m_ui->deselectObject(obj);
        this->untrack(obj);
        bool const previous = m_suppress;
        m_suppress = true;
        m_lel->removeObject(obj, true);
        m_suppress = previous;
    }

    GameObject* Manager::createLocal(std::string const& data) {
        std::string str = data;
        if (str.empty()) return nullptr;
        if (str.back() != ';') str.push_back(';');

        bool const previous = m_suppress;
        m_suppress = true;
        auto* created = m_lel->createObjectsFromString(str, true, true);
        m_suppress = previous;

        if (!created || created->count() == 0) return nullptr;
        return typeinfo_cast<GameObject*>(created->objectAtIndex(0));
    }

    void Manager::applyRemote(RemoteOp const& op) {
        switch (op.kind) {
            case RemoteOp::Kind::Add:
            case RemoteOp::Kind::Update: {
                if (op.data.empty()) return;
                bool reselect = false;
                if (GameObject* existing = this->findById(op.id)) {
                    reselect = existing->m_isSelected;
                    this->removeLocal(existing);
                }
                GameObject* created = this->createLocal(op.data);
                if (!created) return;
                this->track(created, op.id, std::hash<std::string>{}(this->saveString(created)));
                if (reselect && m_ui) m_ui->selectObject(created, true);
            } break;
            case RemoteOp::Kind::Remove: {
                if (GameObject* existing = this->findById(op.id)) {
                    this->removeLocal(existing);
                }
            } break;
        }
    }

    void Manager::stepClear() {
        auto const start = Clock::now();
        while (!m_toClear.empty() && msSince(start) < 8.0) {
            Ref<GameObject> obj = m_toClear.back();
            m_toClear.pop_back();
            this->removeLocal(obj);
        }
        this->setStatus(Status::Syncing, fmt::format("Clearing the editor... {} left", m_toClear.size()));
    }

    void Manager::stepRemote() {
        if (m_remote.empty()) return;
        auto const start = Clock::now();
        while (!m_remote.empty() && msSince(start) < 6.0) {
            RemoteOp op = std::move(m_remote.front());
            m_remote.pop_front();
            this->applyRemote(op);
            if (!m_snapshotDone && op.kind == RemoteOp::Kind::Add) m_downloaded++;
        }
        if (m_status == Status::Syncing && !m_uploading) {
            this->setStatus(
                Status::Syncing,
                fmt::format("Loading level... {} / {}", m_downloaded, m_downloadTotal)
            );
        }
    }

    // ------------------------------------------------------------------
    // detecting local changes
    // ------------------------------------------------------------------

    void Manager::onBeforeRemoveObject(GameObject* obj) {
        if (m_suppress || m_status != Status::Live) return;
        auto it = m_byObj.find(obj);
        if (it == m_byObj.end()) return;
        m_out.push_back(LocalOp { 'd', it->second.id, "" });
        this->untrack(obj);
    }

    void Manager::reconcile() {
        auto* arr = m_lel->m_objects;
        if (!arr) return;

        std::unordered_set<GameObject*> present;
        present.reserve(arr->count());
        for (unsigned int i = 0; i < arr->count(); i++) {
            auto* obj = static_cast<GameObject*>(arr->objectAtIndex(i));
            present.insert(obj);
            if (m_byObj.contains(obj)) continue;

            // a new object that we have not announced yet
            std::string data = this->saveString(obj);
            std::string id = this->newId();
            this->track(obj, id, std::hash<std::string>{}(data));
            if (!data.empty()) m_out.push_back(LocalOp { 'a', id, std::move(data) });
        }

        // objects that disappeared without going through removeObject
        std::vector<GameObject*> gone;
        for (auto const& [obj, info] : m_byObj) {
            if (!present.contains(obj)) gone.push_back(obj);
        }
        for (auto* obj : gone) {
            auto it = m_byObj.find(obj);
            if (it == m_byObj.end()) continue;
            m_out.push_back(LocalOp { 'd', it->second.id, "" });
            this->untrack(obj);
        }
    }

    void Manager::checkObject(GameObject* obj) {
        auto it = m_byObj.find(obj);
        if (it == m_byObj.end()) return;
        std::string data = this->saveString(obj);
        if (data.empty()) return;
        size_t const hash = std::hash<std::string>{}(data);
        if (hash == it->second.hash) return;
        it->second.hash = hash;
        m_out.push_back(LocalOp { 'u', it->second.id, std::move(data) });
    }

    void Manager::scanSelection() {
        // Everything the player can edit is part of the selection, so that is
        // where changes (moves, rotations, property edits...) are looked for.
        // Objects that were selected on the previous scan are checked once more
        // so the final state is sent after they get deselected.
        std::vector<GameObject*> work;
        std::unordered_set<GameObject*> seen;

        if (auto* selected = m_ui->getSelectedObjects()) {
            for (unsigned int i = 0; i < selected->count(); i++) {
                auto* obj = typeinfo_cast<GameObject*>(selected->objectAtIndex(i));
                if (obj && seen.insert(obj).second) work.push_back(obj);
            }
        }
        for (auto* obj : m_prevSelection) {
            // tracked objects are kept alive by the tracking table, so the
            // pointer is only touched if it is still in there
            if (m_byObj.contains(obj) && seen.insert(obj).second) work.push_back(obj);
        }
        m_prevSelection.clear();
        for (auto* obj : work) {
            if (obj->m_isSelected) m_prevSelection.push_back(obj);
        }
        if (work.empty()) return;

        auto const start = Clock::now();
        size_t const count = work.size();
        size_t processed = 0;
        size_t pos = m_selCursor % count;
        while (processed < count && msSince(start) < 4.0) {
            this->checkObject(work[pos]);
            pos = (pos + 1) % count;
            processed++;
        }
        m_selCursor = pos;
    }

    void Manager::scanSweep() {
        // Slow safety net: walks through every object over time to catch
        // changes that did not go through the selection (undo / redo etc.)
        auto* arr = m_lel->m_objects;
        if (!arr || arr->count() == 0) return;

        auto const start = Clock::now();
        size_t checked = 0;
        size_t const total = arr->count();
        while (checked < total && msSince(start) < 2.0) {
            if (m_sweepPos >= total) m_sweepPos = 0;
            auto* obj = static_cast<GameObject*>(arr->objectAtIndex(static_cast<unsigned int>(m_sweepPos++)));
            this->checkObject(obj);
            checked++;
        }
    }

    void Manager::scan() {
        auto* arr = m_lel->m_objects;
        if (!arr) return;

        m_reconcileTimer += 0.2f;
        if (arr->count() != m_byObj.size() || m_reconcileTimer >= 6.f) {
            m_reconcileTimer = 0.f;
            this->reconcile();
        }
        this->scanSelection();
        this->shareSelection();
        this->scanSweep();
    }

    // ------------------------------------------------------------------
    // shared selection
    // ------------------------------------------------------------------

    void Manager::clearSelections() {
        m_remoteSel.clear();
        m_lastSelSig.clear();
        m_mySelCount = 0;
        m_mySelFirst.clear();
        m_overlayTimer = 0.f;
        if (m_overlay && m_overlayShown) m_overlay->setEntries({});
        m_overlayShown = false;
    }

    // Tells the others which objects we have selected (only when it changed).
    void Manager::shareSelection() {
        constexpr size_t kMaxShared = 300;

        std::vector<std::string> ids;
        size_t total = 0;
        if (auto* selected = m_ui->getSelectedObjects()) {
            for (unsigned int i = 0; i < selected->count(); i++) {
                auto* obj = typeinfo_cast<GameObject*>(selected->objectAtIndex(i));
                if (!obj) continue;
                total++;
                if (ids.size() >= kMaxShared) continue;
                if (auto it = m_byObj.find(obj); it != m_byObj.end()) ids.push_back(it->second.id);
            }
        }

        std::string sig;
        for (auto const& id : ids) {
            sig += id;
            sig.push_back(',');
        }
        sig += fmt::format("#{}", total);
        if (sig == m_lastSelSig) return;
        m_lastSelSig = std::move(sig);
        m_mySelCount = total;
        m_mySelFirst = ids.empty() ? std::string() : ids.front();

        auto arr = matjson::Value::array();
        for (auto const& id : ids) arr.push(id);
        this->sendJson(matjson::makeObject({
            {"t", "sel"},
            {"ids", std::move(arr)},
            {"n", static_cast<int>(total)},
        }));
    }

    SelectionInfo Manager::selectionOf(int playerId) const {
        SelectionInfo info;
        std::string first;
        if (playerId == m_clientId) {
            info.count = m_mySelCount;
            first = m_mySelFirst;
        }
        else if (auto it = m_remoteSel.find(playerId); it != m_remoteSel.end()) {
            info.count = it->second.count;
            if (!it->second.ids.empty()) first = it->second.ids.front();
        }
        if (info.count == 1 && !first.empty()) {
            if (GameObject* obj = this->findById(first)) info.objectId = obj->m_objectID;
        }
        return info;
    }

    // Outlines the objects other players have selected.
    void Manager::updateOverlay(float dt) {
        if (!m_overlay) return;

        bool const active = (m_status == Status::Live || m_status == Status::Syncing)
            && cfg::flag("show-selections") && !m_remoteSel.empty() && m_toClear.empty() && !this->isPlaytesting();
        if (!active) {
            if (m_overlayShown) {
                m_overlay->setEntries({});
                m_overlayShown = false;
            }
            return;
        }

        m_overlayTimer += dt;
        if (m_overlayTimer < .1f) return;
        m_overlayTimer = 0.f;

        auto* gm = GameManager::get();
        auto const winSize = CCDirector::get()->getWinSize();
        size_t budget = 400; // boxes drawn in total

        std::vector<OverlayEntry> entries;
        for (auto const& [playerId, sel] : m_remoteSel) {
            int const wantedId = playerId; // (structured bindings can not be captured portably)
            auto player = std::find_if(m_players.begin(), m_players.end(), [wantedId](PlayerInfo const& p) {
                return p.id == wantedId;
            });
            if (player == m_players.end()) continue;

            OverlayEntry entry;
            entry.playerId = playerId;
            entry.label = sel.count == 1 ? player->name : fmt::format("{} - {}", player->name, sel.count);

            // the player's own colour, unless it would vanish on the dark background
            ccColor3B color = gm->colorForIdx(player->c1);
            if (brightness(color) < 70) color = gm->colorForIdx(player->c2);
            if (brightness(color) < 70) color = ccColor3B { 255, 255, 255 };
            entry.color = color;

            for (auto const& id : sel.ids) {
                if (budget == 0) break;
                GameObject* obj = this->findById(id);
                if (!obj) continue;
                auto* parent = obj->getParent();
                if (!parent) continue;

                CCRect const rect = obj->getObjectRect();
                float const w = std::max(rect.size.width, 8.f);
                float const h = std::max(rect.size.height, 8.f);
                CCPoint const a = m_overlay->convertToNodeSpace(
                    parent->convertToWorldSpace(ccp(rect.getMinX(), rect.getMinY()))
                );
                CCPoint const b = m_overlay->convertToNodeSpace(
                    parent->convertToWorldSpace(ccp(rect.getMinX() + w, rect.getMinY() + h))
                );
                CCRect const box(std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y));

                // skip what is not on screen
                if (box.getMaxX() < -20.f || box.getMinX() > winSize.width + 20.f) continue;
                if (box.getMaxY() < -20.f || box.getMinY() > winSize.height + 20.f) continue;

                entry.boxes.push_back(box);
                budget--;
            }
            if (!entry.boxes.empty()) entries.push_back(std::move(entry));
        }
        m_overlay->setEntries(entries);
        m_overlayShown = true;
    }

    // ------------------------------------------------------------------
    // invites
    // ------------------------------------------------------------------

    void Manager::refreshInviteSettings() {
        bool const wanted = cfg::flag("receive-invites");
        std::string server = cfg::server();
        std::string name = cfg::playerName();
        if (wanted == m_lobbyWanted && server == m_lobbyServer && name == m_lobbyName) return;

        m_lobbyWanted = wanted;
        m_lobbyServer = std::move(server);
        m_lobbyName = std::move(name);
        m_lobbyError.clear();
        this->stopLobby();
        m_lobbyRetry = 0.f;
    }

    std::string Manager::inviteStatusText() const {
        if (!m_lobbyWanted) return "Off - friends can not invite you";
        if (m_lobbyServer.empty()) return "Needs a party server - set one in the Config";
        switch (m_lobbyState) {
            case LobbyState::Listening: return fmt::format("Listening for invites as {}", m_lobbyName);
            case LobbyState::Connecting: return "Connecting to the server...";
            default: return m_lobbyError.empty() ? std::string("Waiting to connect...") : m_lobbyError;
        }
    }

    void Manager::startLobby() {
        std::string address, error;
        int port = 5588;
        if (!parseServer(m_lobbyServer, address, port, error)) {
            m_lobbyError = error;
            m_lobbyRetry = 2.f;
            return;
        }
        m_lobbyError.clear();
        m_lobbyState = LobbyState::Connecting;
        m_lobbyPing = 0.f;
        m_lobby.connect(address, port);
    }

    void Manager::stopLobby() {
        m_lobby.close();
        (void)m_lobby.poll(); // drop whatever the old connection still had queued
        m_lobbyState = LobbyState::Off;
    }

    void Manager::tickLobby(float dt) {
        if (m_inviteCooldown > 0.f) m_inviteCooldown -= dt;

        for (auto const& ev : m_lobby.poll()) {
            this->handleLobbyEvent(ev);
        }

        // the lobby is only open while we are not in a room
        if (!m_lobbyWanted || m_status != Status::Offline) {
            if (m_lobbyState != LobbyState::Off) this->stopLobby();
            return;
        }

        if (m_lobbyState == LobbyState::Off) {
            m_lobbyRetry -= dt;
            if (m_lobbyRetry <= 0.f) this->startLobby();
        }
        else if (m_lobbyState == LobbyState::Listening) {
            m_lobbyPing += dt;
            if (m_lobbyPing >= 10.f) {
                m_lobbyPing = 0.f;
                m_lobby.send(matjson::makeObject({{"t", "ping"}}).dump(matjson::NO_INDENTATION));
            }
        }
    }

    void Manager::handleLobbyEvent(NetEvent const& ev) {
        switch (ev.type) {
            case NetEvent::Type::Connected: {
                m_lobby.send(matjson::makeObject({
                    {"t", "hello"},
                    {"ver", kProtocolVersion},
                    {"mode", "lobby"},
                    {"name", m_lobbyName},
                }).dump(matjson::NO_INDENTATION));
            } break;
            case NetEvent::Type::Line: {
                auto parsed = matjson::Value::parse(ev.text);
                if (parsed.isErr()) return;
                matjson::Value const& msg = parsed.unwrap();
                std::string const type = getStr(msg, "t");
                if (type == "lobby") {
                    m_lobbyState = LobbyState::Listening;
                    m_lobbyError.clear();
                }
                else if (type == "invite") {
                    this->showInvite(plain(getStr(msg, "from", "Somebody")), getStr(msg, "room"));
                }
                else if (type == "error") {
                    std::string err = getStr(msg, "msg", "The server refused the connection");
                    // servers from before invites existed treat "lobby" like a room join
                    if (err.find("Invalid room code") != std::string::npos) {
                        err = "This server is too old for invites - update server.js";
                    }
                    this->stopLobby();
                    m_lobbyError = std::move(err);
                    m_lobbyRetry = 20.f;
                }
            } break;
            case NetEvent::Type::Closed: {
                m_lobbyState = LobbyState::Off;
                m_lobbyError = ev.text;
                m_lobbyRetry = 8.f;
            } break;
        }
    }

    void Manager::showInvite(std::string from, std::string room) {
        if (!m_ui || m_status != Status::Offline || m_inviteCooldown > 0.f || room.empty()) return;
        m_inviteCooldown = 30.f; // at most one invitation window at a time

        createQuickPopup(
            "Invitation",
            fmt::format(
                "<cy>{}</c> invited you to the room <cg>{}</c>.\n\n"
                "Joining <cr>deletes everything in this editor</c> and loads the room's level instead. "
                "Save a copy of your level first if you want to keep it!",
                from, plain(room)
            ),
            "Decline", "Join",
            [room](FLAlertLayer*, bool join) {
                auto& self = Manager::get();
                self.m_inviteCooldown = 0.f;
                if (!join || !self.m_ui || self.m_status != Status::Offline) return;
                self.m_skipConfirm = true;
                self.connect(self.m_lobbyServer, room, self.m_lobbyName, false);
            }
        );
    }

    void Manager::invite(std::string const& playerName) {
        std::string const target = trim(playerName);
        if (m_status != Status::Live) {
            Notification::create("Invites can be sent once the room is live", NotificationIcon::Warning, 2.f)->show();
            return;
        }
        if (target.empty()) {
            Notification::create("Type the name of the player you want to invite", NotificationIcon::Warning, 2.f)->show();
            return;
        }
        this->sendJson(matjson::makeObject({{"t", "invite"}, {"to", target}}));
    }

    // ------------------------------------------------------------------
    // per-frame work
    // ------------------------------------------------------------------

    void Manager::flushOps() {
        if (m_out.empty() || m_status != Status::Live) return;

        size_t pos = 0;
        while (pos < m_out.size()) {
            size_t const end = std::min(m_out.size(), pos + kOpsPerMessage);
            auto arr = matjson::Value::array();
            for (size_t i = pos; i < end; i++) {
                auto const& op = m_out[i];
                auto item = matjson::Value::array();
                item.push(std::string(1, op.kind));
                item.push(op.id);
                if (op.kind != 'd') item.push(op.data);
                arr.push(std::move(item));
            }
            this->sendJson(matjson::makeObject({{"t", "ops"}, {"ops", std::move(arr)}}));
            pos = end;
        }
        m_out.clear();
    }

    void Manager::tick(float dt) {
        if (!m_ui || !m_lel) return;

        this->tickLobby(dt);
        this->tickLocalParty();
        for (auto const& ev : m_net.poll()) {
            this->handleEvent(ev);
        }
        this->updateOverlay(dt);
        if (m_status == Status::Offline) return;

        m_pingTimer += dt;
        if (m_pingTimer >= 10.f && m_status != Status::Connecting) {
            m_pingTimer = 0.f;
            this->sendJson(matjson::makeObject({{"t", "ping"}}));
        }

        if (m_status == Status::Connecting || m_status == Status::Joining || m_status == Status::Confirm) {
            return;
        }
        // objects behave differently while playtesting, so do nothing then
        if (this->isPlaytesting()) return;

        if (m_uploading) {
            this->stepUpload();
        }
        if (!m_toClear.empty()) {
            this->stepClear();
            return;
        }
        this->stepRemote();

        if (m_status == Status::Live) {
            if (m_remote.empty()) {
                m_scanTimer += dt;
                if (m_scanTimer >= 0.2f) {
                    m_scanTimer = 0.f;
                    this->scan();
                }
            }
            this->flushOps();
        }
        else if (m_status == Status::Syncing && !m_uploading && m_snapshotDone && m_remote.empty()) {
            if (m_lel->m_undoObjects) m_lel->m_undoObjects->removeAllObjects();
            if (m_lel->m_redoObjects) m_lel->m_redoObjects->removeAllObjects();
            this->setStatus(Status::Live, fmt::format("Live in room {}", m_room));
        }
    }

}
