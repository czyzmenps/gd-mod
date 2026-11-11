#include "CollabPopup.hpp"

#include "Config.hpp"

#include <algorithm>

using namespace geode::prelude;
using collab::Manager;
using collab::Status;

namespace {
    constexpr float kWidth = 380.f;
    constexpr float kHeight = 310.f;

    constexpr float kStatusY = 106.f;

    constexpr float kBarWidth = 300.f;
    constexpr float kBarHeight = 8.f;

    constexpr float kListWidth = 316.f;
    constexpr float kListHeight = 88.f;
    constexpr float kRowHeight = 30.f;

    // position inside a full-size view node, measured from the popup centre
    CCPoint at(float x, float y) {
        return { kWidth / 2.f + x, kHeight / 2.f + y };
    }

    CCNode* makeLayer() {
        auto node = CCNode::create();
        node->setContentSize({ kWidth, kHeight });
        node->setAnchorPoint({ .5f, .5f });
        return node;
    }

    // rounded dark panel, w x h in real pixels
    NineSlice* makePanel(float w, float h, GLubyte opacity, ccColor3B color = { 0, 0, 0 }) {
        auto panel = NineSlice::create("square02b_001.png", { 0.f, 0.f, 80.f, 80.f });
        panel->setAnchorPoint({ .5f, .5f });
        panel->setScale(.5f);
        panel->setContentSize({ w * 2.f, h * 2.f });
        panel->setColor(color);
        panel->setOpacity(opacity);
        return panel;
    }

    CCLabelBMFont* makeLabel(
        char const* text, char const* font, float scale, GLubyte opacity, CCPoint anchor
    ) {
        auto label = CCLabelBMFont::create(text, font);
        label->setScale(scale);
        label->setOpacity(opacity);
        label->setAnchorPoint(anchor);
        return label;
    }

    // shrink a label so it is at most maxWidth wide (never grows past baseScale)
    void fitLabel(CCLabelBMFont* label, float maxWidth, float baseScale) {
        float const raw = label->getContentSize().width;
        label->setScale(raw > 0.f ? std::min(baseScale, maxWidth / raw) : baseScale);
    }

    // A button that is never wider than maxWidth, whatever the font metrics turn
    // out to be: the sprite is built at full size and then scaled down to fit.
    CCMenuItemSpriteExtra* makeButton(
        char const* text, char const* texture, float maxWidth, float maxScale,
        CCObject* target, SEL_MenuHandler handler
    ) {
        auto spr = ButtonSprite::create(text, "bigFont.fnt", texture, 1.f);
        float const natural = spr->getContentSize().width;
        float const scale = natural > 1.f ? std::min(maxScale, maxWidth / natural) : maxScale * .8f;
        spr->setScale(scale);
        return CCMenuItemSpriteExtra::create(spr, target, handler);
    }
}

CollabPopup* CollabPopup::create() {
    auto ret = new CollabPopup();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

CollabPopup::~CollabPopup() {
    if (s_current == this) s_current = nullptr;
}

void CollabPopup::toggle() {
    if (!Manager::get().inEditor()) return;
    if (s_current) {
        s_current->onClose(nullptr);
        return;
    }
    if (auto popup = CollabPopup::create()) popup->show();
}

bool CollabPopup::init() {
    if (!Popup::init(kWidth, kHeight)) return false;
    this->setTitle("Live Collab");

    this->buildStatusRow();
    this->buildConnectView();
    this->buildRoomView();

    this->refresh(0.f);
    this->schedule(schedule_selector(CollabPopup::refresh), 0.2f);
    this->schedule(schedule_selector(CollabPopup::animate));

    s_current = this;
    return true;
}

// ----------------------------------------------------------------------
// building the views
// ----------------------------------------------------------------------

void CollabPopup::buildStatusRow() {
    m_statusRow = makeLayer();
    m_mainLayer->addChildAtPosition(m_statusRow, Anchor::Center);

    m_statusPill = makePanel(130.f, 22.f, 110);
    m_statusPill->setPosition(at(0.f, kStatusY));
    m_statusRow->addChild(m_statusPill);

    m_statusDot = collab::CollabDot::create(4.5f);
    m_statusRow->addChild(m_statusDot, 1);

    m_statusLabel = makeLabel("", "chatFont.fnt", .6f, 255, { 0.f, .5f });
    m_statusRow->addChild(m_statusLabel, 1);
}

void CollabPopup::buildConnectView() {
    m_connectView = makeLayer();
    m_mainLayer->addChildAtPosition(m_connectView, Anchor::Center);

    // the one big button
    m_startBtn = makeButton(
        "Start party", "GJ_button_01.png", 240.f, .95f, this, menu_selector(CollabPopup::onStart)
    );
    m_buttonMenu->addChildAtPosition(m_startBtn, Anchor::Center, ccp(0.f, 58.f));

    auto startNote = makeLabel(
        "Creates a new party and copies its code for your friends", "chatFont.fnt", .5f, 170, { .5f, .5f }
    );
    fitLabel(startNote, 340.f, .5f);
    startNote->setPosition(at(0.f, 28.f));
    m_connectView->addChild(startNote);

    // joining a friend's party
    auto joinCaption = makeLabel("JOIN A PARTY", "bigFont.fnt", .3f, 160, { 0.f, .5f });
    joinCaption->setPosition(at(-155.f, 4.f));
    m_connectView->addChild(joinCaption);

    m_codeInput = TextInput::create(220.f, "Party code");
    m_codeInput->setMaxCharCount(90);
    m_codeInput->setFilter("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789@:.-_");
    m_codeInput->setString(Manager::get().partyCode());
    m_mainLayer->addChildAtPosition(m_codeInput, Anchor::Center, ccp(-45.f, -20.f));

    m_joinBtn = makeButton(
        "Join", "GJ_button_02.png", 90.f, .75f, this, menu_selector(CollabPopup::onJoin)
    );
    m_buttonMenu->addChildAtPosition(m_joinBtn, Anchor::Center, ccp(122.f, -20.f));

    // invites
    m_inviteToggle = CCMenuItemToggler::createWithStandardSprites(
        this, menu_selector(CollabPopup::onToggleInvites), .6f
    );
    m_inviteToggle->toggle(collab::cfg::flag("receive-invites"));
    m_buttonMenu->addChildAtPosition(m_inviteToggle, Anchor::Center, ccp(-160.f, -58.f));

    auto toggleLabel = makeLabel("Receive invites", "bigFont.fnt", .35f, 230, { 0.f, .5f });
    toggleLabel->setPosition(at(-142.f, -58.f));
    m_connectView->addChild(toggleLabel);

    m_inviteStatus = makeLabel("", "chatFont.fnt", .5f, 220, { 0.f, .5f });
    m_inviteStatus->setPosition(at(-155.f, -77.f));
    m_connectView->addChild(m_inviteStatus);

    // who you are and which server is used (both live in the Config)
    m_nameLabel = makeLabel("", "chatFont.fnt", .55f, 230, { 0.f, .5f });
    m_nameLabel->setPosition(at(-155.f, -104.f));
    m_connectView->addChild(m_nameLabel);

    m_serverLabel = makeLabel("", "chatFont.fnt", .5f, 200, { 0.f, .5f });
    m_serverLabel->setPosition(at(-155.f, -120.f));
    m_connectView->addChild(m_serverLabel);

    m_configBtn = makeButton(
        "Config", "GJ_button_04.png", 100.f, .6f, this, menu_selector(CollabPopup::onConfig)
    );
    m_buttonMenu->addChildAtPosition(m_configBtn, Anchor::Center, ccp(125.f, -112.f));

    auto warning = makeLabel(
        "Joining replaces everything in this editor with the party's level.", "chatFont.fnt", .45f, 140, { .5f, .5f }
    );
    fitLabel(warning, 350.f, .45f);
    warning->setPosition(at(0.f, -140.f));
    m_connectView->addChild(warning);
}

void CollabPopup::buildRoomView() {
    m_roomView = makeLayer();
    m_mainLayer->addChildAtPosition(m_roomView, Anchor::Center);

    // room code card
    auto caption = makeLabel("PARTY CODE", "bigFont.fnt", .3f, 160, { 0.f, .5f });
    caption->setPosition(at(-150.f, 88.f));
    m_roomView->addChild(caption);

    auto codePanel = makePanel(240.f, 34.f, 110);
    codePanel->setPosition(at(-30.f, 64.f));
    m_roomView->addChild(codePanel);

    m_codeLabel = makeLabel("", "goldFont.fnt", .85f, 255, { .5f, .5f });
    m_codeLabel->setPosition(at(-30.f, 64.f));
    m_roomView->addChild(m_codeLabel, 1);

    m_serverNote = makeLabel("", "chatFont.fnt", .45f, 170, { 1.f, .5f });
    m_serverNote->setPosition(at(170.f, 88.f));
    m_roomView->addChild(m_serverNote);

    m_copyBtn = makeButton(
        "Copy code", "GJ_button_04.png", 84.f, .6f, this, menu_selector(CollabPopup::onCopy)
    );
    m_buttonMenu->addChildAtPosition(m_copyBtn, Anchor::Center, ccp(135.f, 64.f));

    // sync progress
    m_syncLabel = makeLabel("", "chatFont.fnt", .5f, 210, { .5f, .5f });
    m_syncLabel->setPosition(at(0.f, 34.f));
    m_roomView->addChild(m_syncLabel);

    m_bar = CCDrawNode::create();
    m_bar->setPosition(at(-kBarWidth / 2.f, 18.f));
    m_roomView->addChild(m_bar);

    // players
    m_playersHeader = makeLabel("Players online", "bigFont.fnt", .33f, 200, { 0.f, .5f });
    m_playersHeader->setPosition(at(-160.f, 6.f));
    m_roomView->addChild(m_playersHeader);

    auto listPanel = makePanel(kListWidth + 12.f, kListHeight + 8.f, 100);
    listPanel->setPosition(at(0.f, -42.f));
    m_roomView->addChild(listPanel);

    m_list = ScrollLayer::create(CCSize { kListWidth, kListHeight });
    m_list->m_contentLayer->setLayout(ScrollLayer::createDefaultListLayout(2.f));
    m_list->setTouchEnabled(true);
    m_list->setPosition(at(-kListWidth / 2.f, -86.f));
    m_roomView->addChild(m_list, 1);

    // bottom row: [nick to invite] [Invite]        [Leave room]
    m_inviteInput = TextInput::create(120.f, "Nick to invite");
    m_inviteInput->setMaxCharCount(20);
    m_mainLayer->addChildAtPosition(m_inviteInput, Anchor::Center, ccp(-115.f, -122.f));

    m_inviteBtn = makeButton(
        "Invite", "GJ_button_03.png", 80.f, .6f, this, menu_selector(CollabPopup::onInvite)
    );
    m_buttonMenu->addChildAtPosition(m_inviteBtn, Anchor::Center, ccp(-10.f, -122.f));

    m_leaveBtn = makeButton(
        "Leave room", "GJ_button_06.png", 135.f, .6f, this, menu_selector(CollabPopup::onLeave)
    );
    m_buttonMenu->addChildAtPosition(m_leaveBtn, Anchor::Center, ccp(108.f, -122.f));
}

CCNode* CollabPopup::makePlayerRow(collab::PlayerInfo const& player, bool isMe) {
    auto* gm = GameManager::get();

    auto row = CCNode::create();
    row->setContentSize({ kListWidth, kRowHeight });

    auto bg = makePanel(
        kListWidth, kRowHeight, isMe ? 90 : 55,
        isMe ? ccColor3B { 90, 75, 20 } : ccColor3B { 0, 0, 0 }
    );
    bg->setPosition({ kListWidth / 2.f, kRowHeight / 2.f });
    row->addChild(bg);

    // the player's own icon, in the player's own colours
    if (auto icon = SimplePlayer::create(player.icon)) {
        auto const col1 = gm->colorForIdx(player.c1);
        auto const col2 = gm->colorForIdx(player.c2);
        icon->setColors(col1, col2);
        if (player.glow) icon->setGlowOutline(col2);
        icon->setScale(.7f);
        icon->setPosition({ 22.f, kRowHeight / 2.f });
        row->addChild(icon, 1);
    }

    auto name = makeLabel(player.name.c_str(), "bigFont.fnt", .42f, 255, { 0.f, .5f });
    fitLabel(name, 190.f, .42f);
    name->setPosition({ 44.f, kRowHeight / 2.f + 5.f });
    if (isMe) name->setColor({ 255, 225, 100 });
    row->addChild(name, 1);

    // what this player has selected (filled in by updateSubtitles)
    auto subtitle = makeLabel("", "chatFont.fnt", .45f, 190, { 0.f, .5f });
    subtitle->setPosition({ 44.f, kRowHeight / 2.f - 8.f });
    row->addChild(subtitle, 1);
    m_subtitles[player.id] = SubtitleSlot { subtitle, "" };

    if (isMe) {
        auto tag = makeLabel("YOU", "bigFont.fnt", .3f, 200, { 1.f, .5f });
        tag->setColor({ 255, 225, 100 });
        tag->setPosition({ kListWidth - 10.f, kRowHeight / 2.f });
        row->addChild(tag, 1);
    }
    else {
        auto dot = collab::CollabDot::create(3.5f);
        dot->setStatus(Status::Live);
        dot->setPosition({ kListWidth - 14.f, kRowHeight / 2.f });
        row->addChild(dot, 1);
    }
    return row;
}

// ----------------------------------------------------------------------
// actions
// ----------------------------------------------------------------------

void CollabPopup::startConnection(bool host) {
    auto& manager = Manager::get();
    if (!manager.inEditor()) return;

    std::string server = collab::cfg::server();
    std::string room;

    if (!host) {
        // a party code is "CODE", or "CODE@server" when the host uses their own server
        std::string code = collab::cfg::trimmed(static_cast<std::string>(m_codeInput->getString()));
        if (code.empty()) {
            Notification::create("Type the party code your friend sent you", NotificationIcon::Warning, 2.f)->show();
            return;
        }
        if (auto sep = code.find('@'); sep != std::string::npos) {
            server = collab::cfg::trimmed(code.substr(sep + 1));
            code = collab::cfg::trimmed(code.substr(0, sep));
        }
        room = code;
    }

    manager.connect(server, room, collab::cfg::playerName(), host);
    this->refresh(0.f);
}

void CollabPopup::onStart(CCObject*) {
    this->startConnection(true);
}

void CollabPopup::onJoin(CCObject*) {
    this->startConnection(false);
}

void CollabPopup::onLeave(CCObject*) {
    Manager::get().disconnect("Left the room");
    this->refresh(0.f);
}

void CollabPopup::onCopy(CCObject*) {
    auto const& room = Manager::get().room();
    if (room.empty()) return;
    geode::utils::clipboard::write(Manager::get().partyCode());
    Notification::create("Party code copied", NotificationIcon::Success, 1.f)->show();
}

void CollabPopup::onInvite(CCObject*) {
    Manager::get().invite(static_cast<std::string>(m_inviteInput->getString()));
}

void CollabPopup::onToggleInvites(CCObject*) {
    // the toggler still reports the state from before the click
    bool const on = !m_inviteToggle->isToggled();
    Mod::get()->setSettingValue<bool>("receive-invites", on);
    Manager::get().refreshInviteSettings();
    this->refreshInviteStatus();
}

void CollabPopup::onConfig(CCObject*) {
    geode::openSettingsPopup(Mod::get());
}

void CollabPopup::onClose(CCObject* sender) {
    if (s_current == this) s_current = nullptr;
    Popup::onClose(sender);
}

// ----------------------------------------------------------------------
// keeping the window up to date
// ----------------------------------------------------------------------

void CollabPopup::showMode(bool offline) {
    int const mode = offline ? 0 : 1;
    if (mode == m_mode) return;
    m_mode = mode;

    // party view
    m_connectView->setVisible(offline);
    m_codeInput->setVisible(offline);
    m_codeInput->setEnabled(offline);
    m_startBtn->setVisible(offline);
    m_joinBtn->setVisible(offline);
    m_configBtn->setVisible(offline);
    m_inviteToggle->setVisible(offline);

    // room view
    m_roomView->setVisible(!offline);
    m_copyBtn->setVisible(!offline);
    m_leaveBtn->setVisible(!offline);
    m_inviteBtn->setVisible(!offline);
    m_inviteInput->setVisible(!offline);
    m_inviteInput->setEnabled(!offline);
    m_list->setTouchEnabled(!offline);

    if (!offline) {
        // force everything in the room view to be redrawn
        m_shownCode.clear();
        m_shownServerNote.clear();
        m_listSignature.clear();
        m_barPercent = -2;
        m_barPhase = 0.f;
    }
    else {
        // keep the last party code handy for a quick rejoin
        auto const code = Manager::get().partyCode();
        if (!code.empty()) m_codeInput->setString(code);
    }
}

void CollabPopup::setStatusLine(std::string const& text, Status status, ccColor3B color) {
    m_statusDot->setStatus(status);

    int const colorKey = (color.r << 16) | (color.g << 8) | color.b;
    if (text == m_shownStatus && colorKey == m_shownColor) return;
    m_shownStatus = text;
    m_shownColor = colorKey;

    m_statusLabel->setString(text.c_str());
    m_statusLabel->setColor(color);
    fitLabel(m_statusLabel, 290.f, .6f);

    float const labelWidth = m_statusLabel->getContentSize().width * m_statusLabel->getScale();
    float const group = 9.f + 8.f + labelWidth; // dot + gap + text
    float const start = -group / 2.f;

    m_statusDot->setPosition(at(start + 4.5f, kStatusY));
    m_statusLabel->setPosition(at(start + 17.f, kStatusY));
    m_statusPill->setContentSize({ std::max(130.f, group + 28.f) * 2.f, 44.f });
}

// "Playing as ..." and "Server: ..." (both come from the Config)
void CollabPopup::refreshConnectInfo() {
    std::string const name = "Playing as " + collab::cfg::playerName();
    if (name != m_shownName) {
        m_shownName = name;
        m_nameLabel->setString(name.c_str());
        fitLabel(m_nameLabel, 270.f, .55f);
    }

    std::string const server = collab::cfg::server();
    std::string const text = server.empty()
        ? "No party server set - press Config and enter it once"
        : "Server: " + server;
    if (text != m_shownServer) {
        m_shownServer = text;
        m_serverLabel->setString(text.c_str());
        fitLabel(m_serverLabel, 250.f, .5f);
        m_serverLabel->setColor(server.empty() ? ccColor3B { 255, 175, 110 } : ccColor3B { 170, 170, 170 });
    }

    // the switch can also be flipped in the Config while this window is open
    bool const wanted = collab::cfg::flag("receive-invites");
    if (m_inviteToggle->isToggled() != wanted) m_inviteToggle->toggle(wanted);
}

void CollabPopup::refreshInviteStatus() {
    auto& manager = Manager::get();
    std::string const text = manager.inviteStatusText();
    if (text == m_shownInviteStatus) return;
    m_shownInviteStatus = text;

    m_inviteStatus->setString(text.c_str());
    fitLabel(m_inviteStatus, 320.f, .5f);

    ccColor3B color = { 170, 170, 170 };                     // off
    if (manager.invitesListening()) color = { 150, 255, 160 }; // listening
    else if (manager.invitesEnabled()) color = { 255, 200, 90 };
    m_inviteStatus->setColor(color);
}

void CollabPopup::refresh(float) {
    auto& manager = Manager::get();
    bool const offline = manager.isOffline();
    this->showMode(offline);

    // ---- status line ----
    std::string text = manager.statusText();
    ccColor3B color = { 255, 255, 255 };
    if (!manager.inEditor()) {
        text = "Open this window from inside the level editor";
    }
    else if (manager.status() == Status::Live) {
        text = "Live";
        color = { 150, 255, 160 };
    }
    else if (offline) {
        // "Not connected" is calm, anything else is a reason worth noticing
        color = text == "Not connected" ? ccColor3B { 170, 170, 170 } : ccColor3B { 255, 175, 110 };
    }
    this->setStatusLine(text, manager.status(), color);

    if (offline) {
        this->refreshConnectInfo();
        this->refreshInviteStatus();
        return;
    }
    this->refreshRoomView();
}

void CollabPopup::refreshRoomView() {
    auto& manager = Manager::get();

    // big room code
    if (manager.room() != m_shownCode) {
        m_shownCode = manager.room();
        m_codeLabel->setString(m_shownCode.c_str());
        fitLabel(m_codeLabel, 220.f, .85f);
    }

    // which server the party runs on
    std::string const note = manager.server().empty() ? std::string() : "on " + manager.server();
    if (note != m_shownServerNote) {
        m_shownServerNote = note;
        m_serverNote->setString(note.c_str());
        fitLabel(m_serverNote, 190.f, .45f);
    }

    // line above the progress bar
    std::string sync;
    switch (manager.status()) {
        case Status::Live: sync = "In sync - everything you build is shared instantly"; break;
        default: sync = manager.statusText(); break;
    }
    m_syncLabel->setString(sync.c_str());
    fitLabel(m_syncLabel, 310.f, .5f);

    // players
    auto const& players = manager.players();
    m_playersHeader->setString(fmt::format("Players online: {}", std::max<size_t>(1, players.size())).c_str());

    std::string signature = fmt::format("{}|", manager.clientId());
    for (auto const& p : players) {
        signature += fmt::format("{},{},{},{},{},{};", p.id, p.name, p.icon, p.c1, p.c2, p.glow ? 1 : 0);
    }
    if (signature != m_listSignature) {
        m_listSignature = std::move(signature);
        this->rebuildPlayerList();
    }
    this->updateSubtitles();
}

void CollabPopup::rebuildPlayerList() {
    auto& manager = Manager::get();

    m_subtitles.clear(); // the labels are about to be destroyed with their rows
    auto* content = m_list->m_contentLayer;
    content->removeAllChildren();
    for (auto const& player : manager.players()) {
        content->addChild(this->makePlayerRow(player, player.id == manager.clientId()));
    }
    content->updateLayout();
    m_list->scrollToTop();
}

// "Selecting 3 objects" under each name; only touches a label when its text changed
void CollabPopup::updateSubtitles() {
    auto& manager = Manager::get();
    for (auto& [playerId, slot] : m_subtitles) {
        auto const sel = manager.selectionOf(playerId);

        std::string text;
        if (sel.count == 1) {
            text = sel.objectId >= 0 ? fmt::format("Selecting object ID {}", sel.objectId) : "Selecting 1 object";
        }
        else if (sel.count > 1) {
            text = fmt::format("Selecting {} objects", sel.count);
        }

        if (text == slot.text) continue;
        slot.text = text;
        slot.label->setString(text.c_str());
    }
}

// runs every frame: only the progress bar needs that
void CollabPopup::animate(float dt) {
    if (m_mode != 1) return;
    auto& manager = Manager::get();

    Status const status = manager.status();
    float progress = -1.f; // unknown -> bouncing block
    if (status == Status::Live) progress = 1.f;
    else if (status == Status::Syncing) progress = manager.progress();

    int const percent = progress < 0.f ? -1 : static_cast<int>(progress * 100.f);
    if (percent < 0) m_barPhase += dt * .7f;
    else if (percent == m_barPercent && status == m_barStatus) return;

    m_barPercent = percent;
    m_barStatus = status;
    collab::drawProgressBar(m_bar, kBarWidth, kBarHeight, progress, m_barPhase, collab::statusColor(status));
}
