#pragma once

#include <Geode/Geode.hpp>
#include <Geode/ui/ScrollLayer.hpp>

#include <string>
#include <unordered_map>

#include "Collab.hpp"

// The "Live Collab" window that opens from the editor button.
//
// It has two views that swap automatically:
//   * connect view - server / name / room code and the Host / Join buttons
//   * room view    - room code with a copy button, sync progress, the list of
//                    players (with their icons) and a Leave button
class CollabPopup : public geode::Popup {
protected:
    // ---- shared ----
    cocos2d::CCNode* m_statusRow = nullptr;
    geode::NineSlice* m_statusPill = nullptr;
    collab::CollabDot* m_statusDot = nullptr;
    cocos2d::CCLabelBMFont* m_statusLabel = nullptr;
    std::string m_shownStatus;
    int m_shownColor = -1;

    // ---- connect view ----
    cocos2d::CCNode* m_connectView = nullptr;
    geode::TextInput* m_serverInput = nullptr;
    geode::TextInput* m_roomInput = nullptr;
    geode::TextInput* m_nameInput = nullptr;
    CCMenuItemSpriteExtra* m_hostBtn = nullptr;
    CCMenuItemSpriteExtra* m_joinBtn = nullptr;
    CCMenuItemToggler* m_inviteToggle = nullptr;
    cocos2d::CCLabelBMFont* m_inviteStatus = nullptr;
    std::string m_shownInviteStatus;

    // ---- room view ----
    cocos2d::CCNode* m_roomView = nullptr;
    cocos2d::CCLabelBMFont* m_codeLabel = nullptr;
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
    int m_mode = -1; // 0 = connect view, 1 = room view
    std::string m_shownCode;
    std::string m_listSignature;
    std::string m_savedRoom;
    int m_barPercent = -2;
    collab::Status m_barStatus = collab::Status::Offline;
    float m_barPhase = 0.f;

    bool init();

    void buildStatusRow();
    void buildConnectView();
    void buildRoomView();

    void showMode(bool offline);
    void setStatusLine(std::string const& text, collab::Status status, cocos2d::ccColor3B color);
    void refreshRoomView();
    void rebuildPlayerList();
    void updateSubtitles();
    void saveSettings();
    void refreshInviteStatus();
    cocos2d::CCNode* makePlayerRow(collab::PlayerInfo const& player, bool isMe);

    void onHost(cocos2d::CCObject*);
    void onJoin(cocos2d::CCObject*);
    void onLeave(cocos2d::CCObject*);
    void onCopy(cocos2d::CCObject*);
    void onInvite(cocos2d::CCObject*);
    void onToggleInvites(cocos2d::CCObject*);
    void onClose(cocos2d::CCObject*) override;
    void startConnection(bool host);

    void refresh(float dt);
    void animate(float dt);

public:
    static CollabPopup* create();
};
