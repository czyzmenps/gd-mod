#include <Geode/Geode.hpp>

#include <Geode/modify/EditorUI.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>

#include "Collab.hpp"
#include "CollabPopup.hpp"

using namespace geode::prelude;

// Invisible node that drives the collab manager once per frame while the
// editor is open, and shuts it down when the editor is left.
class CollabTicker : public CCNode {
public:
    static CollabTicker* create() {
        auto ret = new CollabTicker();
        if (ret->init()) {
            ret->autorelease();
            ret->scheduleUpdate();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void update(float dt) override {
        collab::Manager::get().tick(dt);
    }

    void onExit() override {
        collab::Manager::get().detach();
        CCNode::onExit();
    }
};

class $modify(CollabEditorUI, EditorUI) {
    bool init(LevelEditorLayer* layer) {
        if (!EditorUI::init(layer)) return false;

        collab::Manager::get().attach(this, layer);

        auto const winSize = CCDirector::get()->getWinSize();

        if (auto ticker = CollabTicker::create()) {
            this->addChild(ticker);
        }

        // pill at the top of the screen: [dot] ROOM - 3 online / sync progress
        auto hud = collab::CollabHud::create();
        hud->setPosition({ winSize.width / 2.f, winSize.height - 4.f });
        this->addChild(hud, 100);

        // the button that opens the collab window, with a status light on it
        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        auto button = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Collab", "bigFont.fnt", "GJ_button_01.png", .6f),
            this, menu_selector(CollabEditorUI::onCollab)
        );
        button->setPosition({ 42.f, winSize.height / 2.f + 30.f });
        menu->addChild(button);
        this->addChild(menu, 100);

        auto badge = collab::CollabDot::create(4.f);
        auto const size = button->getContentSize();
        badge->setPosition(button->getPosition() + CCPoint { size.width / 2.f - 3.f, size.height / 2.f - 3.f });
        this->addChild(badge, 101);

        collab::Manager::get().setHud(hud, badge);

        // outlines around what the other players have selected; z -1 keeps it
        // below the editor's own buttons and panels
        if (auto overlay = collab::SelectionOverlay::create()) {
            this->addChild(overlay, -1);
            collab::Manager::get().setOverlay(overlay);
        }

        return true;
    }

    void onCollab(CCObject*) {
        CollabPopup::create()->show();
    }
};

class $modify(CollabEditorLayer, LevelEditorLayer) {
    void removeObject(GameObject* object, bool noUndo) {
        // keep the object alive until we are completely done with it
        Ref<GameObject> hold = object;
        collab::Manager::get().onBeforeRemoveObject(object);
        LevelEditorLayer::removeObject(object, noUndo);
    }
};
