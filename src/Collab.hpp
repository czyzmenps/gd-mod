#pragma once

#include <Geode/Geode.hpp>

#include "Net.hpp"

#include <chrono>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace collab {

    enum class Status {
        Offline,    // not connected
        Connecting, // TCP connect in progress
        Joining,    // connected, waiting for the server to accept us
        Confirm,    // waiting for the player to confirm replacing their level
        Syncing,    // uploading / downloading the level
        Live        // fully synced, changes flow both ways
    };

    struct PlayerInfo {
        int id = 0;
        std::string name;
    };

    // One change coming from another player.
    struct RemoteOp {
        enum class Kind { Add, Update, Remove };
        Kind kind;
        std::string id;
        std::string data; // GD object string for Add / Update
    };

    // One change made by us that still has to be sent.
    struct LocalOp {
        char kind; // 'a' add, 'u' update, 'd' delete
        std::string id;
        std::string data;
    };

    class Manager {
    public:
        static Manager& get();

        // ---- editor lifetime (called from the hooks) ----
        void attach(EditorUI* ui, LevelEditorLayer* layer);
        void detach();
        void tick(float dt);
        void onBeforeRemoveObject(GameObject* obj);
        void setHud(cocos2d::CCLabelBMFont* label);

        // ---- used by the popup ----
        // mode is "host" (create a new room) or "join" (enter an existing room)
        void connect(std::string server, std::string room, std::string name, bool host);
        void disconnect(std::string const& reason = "Disconnected");

        Status status() const { return m_status; }
        bool isOffline() const { return m_status == Status::Offline; }
        std::string const& statusText() const { return m_statusText; }
        std::string const& room() const { return m_room; }
        std::vector<PlayerInfo> const& players() const { return m_players; }
        bool inEditor() const { return m_ui != nullptr; }

        static std::string makeRoomCode();

    private:
        Manager() = default;

        using Clock = std::chrono::steady_clock;
        static double msSince(Clock::time_point start);

        // networking
        void handleEvent(NetEvent const& ev);
        void handleMessage(std::string const& line);
        void sendJson(matjson::Value const& msg);
        void flushOps();
        void askToReplaceLevel(int objectCount);
        void beginDownload();
        void beginUpload();
        void setStatus(Status status, std::string text);

        // object bookkeeping
        struct ObjInfo {
            std::string id;
            size_t hash = 0;
            geode::Ref<GameObject> keep; // keeps the object alive while it is tracked
        };

        std::string saveString(GameObject* obj) const;
        std::string newId();
        void track(GameObject* obj, std::string id, size_t hash);
        void untrack(GameObject* obj);
        void clearTracking();
        GameObject* findById(std::string const& id) const;
        bool isPlaytesting() const;

        // applying remote changes
        void removeLocal(GameObject* obj);
        GameObject* createLocal(std::string const& data);
        void applyRemote(RemoteOp const& op);
        void stepClear();
        void stepRemote();
        void stepUpload();

        // detecting local changes
        void reconcile();
        void checkObject(GameObject* obj);
        void scanSelection();
        void scanSweep();
        void scan();

        void updateHud();

        // editor state (raw pointers: valid between attach() and detach())
        EditorUI* m_ui = nullptr;
        LevelEditorLayer* m_lel = nullptr;
        geode::Ref<cocos2d::CCLabelBMFont> m_hud;

        // connection state
        NetClient m_net;
        Status m_status = Status::Offline;
        std::string m_statusText = "Not connected";
        std::string m_room;
        std::string m_name;
        bool m_hosting = false;
        int m_clientId = 0;
        uint32_t m_counter = 0;
        std::vector<PlayerInfo> m_players;
        float m_pingTimer = 0.f;

        // tracked objects
        std::unordered_map<GameObject*, ObjInfo> m_byObj;
        std::unordered_map<std::string, GameObject*> m_byId;

        // queues
        std::deque<RemoteOp> m_remote;
        std::vector<LocalOp> m_out;
        std::vector<geode::Ref<GameObject>> m_toClear;
        bool m_snapshotDone = false;
        bool m_suppress = false; // true while we change objects because of remote data

        // upload (host)
        bool m_uploading = false;
        std::vector<geode::Ref<GameObject>> m_uploadList;
        size_t m_uploadPos = 0;
        size_t m_downloaded = 0;
        size_t m_downloadTotal = 0;
        std::string m_lastHud;

        // scanning
        float m_scanTimer = 0.f;
        float m_reconcileTimer = 0.f;
        size_t m_sweepPos = 0;
        size_t m_selCursor = 0;
        std::vector<GameObject*> m_prevSelection; // only ever dereferenced if still tracked
    };

}
