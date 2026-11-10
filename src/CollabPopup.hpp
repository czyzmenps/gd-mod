#pragma once

#include <Geode/Geode.hpp>

// The "Live Collab" window that opens from the editor button.
class CollabPopup : public geode::Popup {
protected:
    geode::TextInput* m_serverInput = nullptr;
    geode::TextInput* m_roomInput = nullptr;
    geode::TextInput* m_nameInput = nullptr;
    cocos2d::CCLabelBMFont* m_statusLabel = nullptr;
    cocos2d::CCLabelBMFont* m_playersLabel = nullptr;
    CCMenuItemSpriteExtra* m_hostBtn = nullptr;
    CCMenuItemSpriteExtra* m_joinBtn = nullptr;
    CCMenuItemSpriteExtra* m_leaveBtn = nullptr;
    CCMenuItemSpriteExtra* m_copyBtn = nullptr;

    bool init();
    void onHost(cocos2d::CCObject*);
    void onJoin(cocos2d::CCObject*);
    void onLeave(cocos2d::CCObject*);
    void onCopy(cocos2d::CCObject*);
    void startConnection(bool host);
    void refresh(float dt);

public:
    static CollabPopup* create();
};
