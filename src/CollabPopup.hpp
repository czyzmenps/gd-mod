#pragma once

#include <Geode/Geode.hpp>
#include <Geode/ui/ScrollLayer.hpp>

#include <string>
#include <unordered_map>

#include "Collab.hpp"

// The "Live Collab" window.
//
// It has two views that swap automatically:
//   * party view  - "Start party", a box for the code of a friend's party,
//                   the invite switch and a button that opens the Config
//   * room view   - party code with a copy button, sync progress, the list of
//                   players (with their icons), invite box and a Leave button
//
// There is no server address in here: the server comes from the mod's Config.
class CollabPopup : public geode::Popup {
protected:
    // ---- shared ----
    cocos2d::CCNode* m_statusRow = nullptr;
    geode::NineSlice* m_statusPill = nullptr;
    collab::CollabDot* m_statusDot = nullptr;
    cocos2d::CCLabelBMFont* m_statusLabel = nullptr;
    std::string m_shownStatus;
    int m_shownColor = -1;

    // ---- party view (not in a party) ----
    cocos2d::CCNode* m_connectView = nullptr;
    cocos2d::CCLabelBMFont* m_startNote = nullptr;
    std::string m_shownNote;
    geode::TextInput* m_codeInput = nullptr;
    CCMenuItemSpriteExtra* m_startBtn = nullptr;
    CCMenuItemSpriteExtra* m_joinBtn = nullptr;
    CCMenuItemSpriteExtra* m_configBtn = nullptr;
    CCMenuItemToggler* m_inviteToggle = nullptr;
    cocos2d::CCLabelBMFont* m_inviteStatus = nullptr;
    cocos2d::CCLabelBMFont* m_nameLabel = nullptr;
    cocos2d::CCLabelBMFont* m_serverLabel = nullptr;
    std::string m_shownInviteStatus;
    std::string m_shownName;
    std::string m_shownServer;

    // ---- room view (in a party) ----
    cocos2d::CCNode* m_roomView = nullptr;
    cocos2d::CCLabelBMFont* m_codeLabel = nullptr;
    cocos2d::CCLabelBMFont* m_serverNote = nullptr;
    cocos2d::CCLabelBMFont* m_syncLabel = nullptr;
    cocos2d::CCLabelBMFont* m_playersHeader = nullptr;
    cocos2d::CCDrawNode* m_bar = nullptr;
    geode::ScrollLayer* m_list = nullptr;
    CCMenuItemSpriteExtra* m_copyBtn = nullptr;
    CCMenuItemSpriteExtra* m_leaveBtn = nullptr;
    geode::TextInput* m_inviteInput = nullptr;
    CCMenuItemSpriteExtra* m_inviteBtn = nullptr;

    // the grey line under each player's name ("Selecting 3 objects")
    struct SubtitleSlot {
        cocos2d::CCLabelBMFont* label = nullptr;
        std::string text;
    };
    std::unordered_map<int, SubtitleSlot> m_subtitles;

    // ---- state ----
    int m_mode = -1; // 0 = party view, 1 = room view
    std::string m_shownCode;
    std::string m_shownServerNote;
    std::string m_listSignature;
    int m_barPercent = -2;
    collab::Status m_barStatus = collab::Status::Offline;
    float m_barPhase = 0.f;

    inline static CollabPopup* s_current = nullptr;

    bool init();
    ~CollabPopup() override;

    void buildStatusRow();
    void buildConnectView();
    void buildRoomView();

    void showMode(bool offline);
    void setStatusLine(std::string const& text, collab::Status status, cocos2d::ccColor3B color);
    void refreshConnectInfo();
    void refreshInviteStatus();
    void refreshRoomView();
    void rebuildPlayerList();
    void updateSubtitles();
    cocos2d::CCNode* makePlayerRow(collab::PlayerInfo const& player, bool isMe);

    void onStart(cocos2d::CCObject*);
    void onJoin(cocos2d::CCObject*);
    void onLeave(cocos2d::CCObject*);
    void onCopy(cocos2d::CCObject*);
    void onInvite(cocos2d::CCObject*);
    void onToggleInvites(cocos2d::CCObject*);
    void onConfig(cocos2d::CCObject*);
    void onClose(cocos2d::CCObject*) override;
    void startConnection(bool host);

    void refresh(float dt);
    void animate(float dt);

public:
    static CollabPopup* create();

    // opens the window, or closes it when it is already open (used by the
    // button and by the shortcut)
    static void toggle();
};
