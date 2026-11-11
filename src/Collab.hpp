#pragma once

#include <Geode/Geode.hpp>

#include "CollabHud.hpp"
#include "CollabLauncher.hpp"
#include "CollabOverlay.hpp"
#include "LocalServer.hpp"
#include "Net.hpp"
#include "Status.hpp"
#include "Upnp.hpp"

#include <chrono>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace collab {

    struct PlayerInfo {
        int id = 0;
        std::string name;
        int icon = 1;  // cube icon id
        int c1 = 0;    // primary colour index
        int c2 = 3;    // secondary colour index
        bool glow = false;
    };

    // What a player has selected right now.
    struct SelectionInfo {
        size_t count = 0;
        int objectId = -1; // GD object id, only filled in when exactly one object is selected
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
        // the pill at the top of the editor and the light on the editor button
        void setHud(CollabHud* hud, CollabDot* badge);
        // draws the outlines of what the other players have selected
        void setOverlay(SelectionOverlay* overlay);
        // the round button in the corner of the editor
        void setLauncher(CollabLauncher* launcher);
        // called when one of the mod's settings changed
        void onSettingChanged(std::string_view key);

        // ---- used by the popup ----
        // Host (host = true): with an empty `server` this game itself becomes the
        // party server (opening the port in the router), otherwise the party is
        // created on that server. Join (host = false): enter the room on `server`.
        void connect(std::string server, std::string room, std::string name, bool host);
        void disconnect(std::string const& reason = "Disconnected");

        Status status() const { return m_status; }
        bool isOffline() const { return m_status == Status::Offline; }
        std::string const& statusText() const { return m_statusText; }
        std::string const& room() const { return m_room; }
        std::string const& server() const { return m_serverLabel; }
        // what you send to your friends: the room code, plus "@server" when the
        // friends could not know the server themselves
        std::string partyCode() const;
        // true while this game itself is the party server
        bool hostedHere() const { return m_localActive; }
        // a longer explanation of what went wrong and what to do (may be empty)
        std::string const& problemHint() const { return m_hint; }
        int clientId() const { return m_clientId; }
        // 0..1 while the level is being uploaded / downloaded, negative when unknown
        float progress() const;
        std::vector<PlayerInfo> const& players() const { return m_players; }
        bool inEditor() const { return m_ui != nullptr; }

        // what the given player (client id) has selected; works for yourself too
        SelectionInfo selectionOf(int playerId) const;

        // ---- invites ----
        // With "Receive invites" on in the Config, a second connection to the
        // server lets other players invite you by name. Call this after the
        // server, the name or that switch changed.
        void refreshInviteSettings();
        bool invitesEnabled() const { return m_lobbyWanted; }
        std::string inviteStatusText() const;
        bool invitesListening() const { return m_lobbyState == LobbyState::Listening; }
        // asks the server to deliver an invite to this room to the player with that name
        void invite(std::string const& playerName);

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

        // the party server inside this game
        void beginSession(bool host, std::string room, std::string name);
        void startLocalParty(std::string room, std::string name);
        void tickLocalParty();
        void connectLocal();
        void stopLocalParty();

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
        std::string hudText() const;

        // shared selection
        void shareSelection();
        void updateOverlay(float dt);
        void clearSelections();

        // invites
        enum class LobbyState { Off, Connecting, Listening };
        void tickLobby(float dt);
        void startLobby();
        void stopLobby();
        void handleLobbyEvent(NetEvent const& ev);
        void showInvite(std::string from, std::string room);
        void announcePlayers(std::vector<PlayerInfo> const& before);

        // editor state (raw pointers: valid between attach() and detach())
        EditorUI* m_ui = nullptr;
        LevelEditorLayer* m_lel = nullptr;
        geode::Ref<CollabHud> m_hud;
        geode::Ref<CollabDot> m_badge;
        geode::Ref<SelectionOverlay> m_overlay;
        geode::Ref<CollabLauncher> m_launcher;

        // the party server inside this game (used when no server is set)
        enum class LocalStage { None, OpeningPort };
        LocalServer m_local;
        PortMapper m_mapper;
        LocalStage m_localStage = LocalStage::None;
        bool m_localActive = false;
        int m_localPort = 0;
        std::string m_localAddress; // "ip:port" that friends use
        std::string m_hint;

        // connection state
        NetClient m_net;
        Status m_status = Status::Offline;
        std::string m_statusText = "Not connected";
        std::string m_room;
        std::string m_name;
        std::string m_serverLabel;
        bool m_hosting = false;
        int m_clientId = 0;
        uint32_t m_counter = 0;
        std::vector<PlayerInfo> m_players;
        float m_pingTimer = 0.f;

        // shared selection
        struct RemoteSelection {
            std::vector<std::string> ids;
            size_t count = 0;
        };
        std::unordered_map<int, RemoteSelection> m_remoteSel;
        std::string m_lastSelSig;
        size_t m_mySelCount = 0;
        std::string m_mySelFirst;
        float m_overlayTimer = 0.f;
        bool m_overlayShown = false;

        // invites
        NetClient m_lobby;
        LobbyState m_lobbyState = LobbyState::Off;
        bool m_lobbyWanted = false;
        std::string m_lobbyServer;
        std::string m_lobbyName;
        std::string m_lobbyError;
        float m_lobbyRetry = 0.f;
        float m_lobbyPing = 0.f;
        float m_inviteCooldown = 0.f; // > 0 while an invitation window is (probably) open
        bool m_skipConfirm = false;   // the invitation window already warned about replacing the level

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

        // scanning
        float m_scanTimer = 0.f;
        float m_reconcileTimer = 0.f;
        size_t m_sweepPos = 0;
        size_t m_selCursor = 0;
        std::vector<GameObject*> m_prevSelection; // only ever dereferenced if still tracked
    };

}
