#include "CollabPopup.hpp"

#include "Collab.hpp"

using namespace geode::prelude;

CollabPopup* CollabPopup::create() {
    auto ret = new CollabPopup();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

bool CollabPopup::init() {
    if (!Popup::init(360.f, 300.f)) return false;
    this->setTitle("Live Collab");

    auto addLabel = [this](char const* text, float y) {
        auto label = CCLabelBMFont::create(text, "bigFont.fnt");
        label->setScale(.4f);
        label->setOpacity(190);
        m_mainLayer->addChildAtPosition(label, Anchor::Center, ccp(0.f, y));
    };

    auto* mod = Mod::get();

    // --- inputs ---
    addLabel("Server (address:port)", 100.f);
    m_serverInput = TextInput::create(250.f, "your.server.com:5588");
    m_serverInput->setMaxCharCount(80);
    m_serverInput->setString(mod->getSavedValue<std::string>("server", ""));
    m_mainLayer->addChildAtPosition(m_serverInput, Anchor::Center, ccp(0.f, 80.f));

    addLabel("Room code (leave empty to create a random one)", 50.f);
    m_roomInput = TextInput::create(250.f, "ROOM CODE");
    m_roomInput->setMaxCharCount(16);
    m_roomInput->setCommonFilter(CommonFilter::Alphanumeric);
    m_roomInput->setString(mod->getSavedValue<std::string>("room", ""));
    m_mainLayer->addChildAtPosition(m_roomInput, Anchor::Center, ccp(0.f, 30.f));

    addLabel("Your name", 0.f);
    m_nameInput = TextInput::create(250.f, "Player");
    m_nameInput->setMaxCharCount(20);
    m_nameInput->setString(mod->getSavedValue<std::string>("name", ""));
    m_mainLayer->addChildAtPosition(m_nameInput, Anchor::Center, ccp(0.f, -20.f));

    // --- buttons ---
    m_hostBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Host new room", .7f), this, menu_selector(CollabPopup::onHost)
    );
    m_buttonMenu->addChildAtPosition(m_hostBtn, Anchor::Center, ccp(-80.f, -68.f));

    m_joinBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Join room", .7f), this, menu_selector(CollabPopup::onJoin)
    );
    m_buttonMenu->addChildAtPosition(m_joinBtn, Anchor::Center, ccp(80.f, -68.f));

    m_leaveBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Leave room", .7f), this, menu_selector(CollabPopup::onLeave)
    );
    m_buttonMenu->addChildAtPosition(m_leaveBtn, Anchor::Center, ccp(-60.f, -68.f));

    m_copyBtn = CCMenuItemSpriteExtra::create(
        ButtonSprite::create("Copy code", .7f), this, menu_selector(CollabPopup::onCopy)
    );
    m_buttonMenu->addChildAtPosition(m_copyBtn, Anchor::Center, ccp(80.f, -68.f));

    // --- status ---
    m_statusLabel = CCLabelBMFont::create("", "chatFont.fnt");
    m_statusLabel->setScale(.55f);
    m_statusLabel->setWidth(560.f);
    m_mainLayer->addChildAtPosition(m_statusLabel, Anchor::Center, ccp(0.f, -108.f));

    m_playersLabel = CCLabelBMFont::create("", "chatFont.fnt");
    m_playersLabel->setScale(.5f);
    m_playersLabel->setOpacity(200);
    m_playersLabel->setWidth(620.f);
    m_mainLayer->addChildAtPosition(m_playersLabel, Anchor::Center, ccp(0.f, -128.f));

    this->refresh(0.f);
    this->schedule(schedule_selector(CollabPopup::refresh), 0.25f);
    return true;
}

void CollabPopup::startConnection(bool host) {
    auto& manager = collab::Manager::get();
    if (!manager.inEditor()) return;

    std::string server = static_cast<std::string>(m_serverInput->getString());
    std::string room = static_cast<std::string>(m_roomInput->getString());
    std::string name = static_cast<std::string>(m_nameInput->getString());

    auto* mod = Mod::get();
    mod->setSavedValue<std::string>("server", server);
    mod->setSavedValue<std::string>("name", name);

    manager.connect(server, room, name, host);
    this->refresh(0.f);
}

void CollabPopup::onHost(CCObject*) {
    this->startConnection(true);
}

void CollabPopup::onJoin(CCObject*) {
    this->startConnection(false);
}

void CollabPopup::onLeave(CCObject*) {
    collab::Manager::get().disconnect("Left the room");
    this->refresh(0.f);
}

void CollabPopup::onCopy(CCObject*) {
    auto const& room = collab::Manager::get().room();
    if (room.empty()) return;
    geode::utils::clipboard::write(room);
    Notification::create("Room code copied", NotificationIcon::Success, 1.f)->show();
}

void CollabPopup::refresh(float) {
    auto& manager = collab::Manager::get();
    bool const offline = manager.isOffline();
    bool const live = manager.status() == collab::Status::Live;

    m_hostBtn->setVisible(offline);
    m_joinBtn->setVisible(offline);
    m_leaveBtn->setVisible(!offline);
    m_copyBtn->setVisible(!offline);

    m_serverInput->setEnabled(offline);
    m_roomInput->setEnabled(offline);
    m_nameInput->setEnabled(offline);

    if (!offline && static_cast<std::string>(m_roomInput->getString()) != manager.room()) {
        m_roomInput->setString(manager.room());
    }
    if (!offline) {
        Mod::get()->setSavedValue<std::string>("room", manager.room());
    }

    std::string statusText = manager.statusText();
    if (!manager.inEditor()) statusText = "Open this window from inside the level editor";
    if (live) statusText = fmt::format("Connected - share the code {} with your team", manager.room());
    m_statusLabel->setString(statusText.c_str());
    m_statusLabel->setColor(live ? ccColor3B { 120, 255, 120 } : ccColor3B { 255, 255, 255 });

    std::string players;
    if (!offline) {
        players = "Online: ";
        bool first = true;
        for (auto const& p : manager.players()) {
            if (!first) players += ", ";
            players += p.name;
            first = false;
        }
        if (first) players += "-";
    }
    m_playersLabel->setString(players.c_str());
}
