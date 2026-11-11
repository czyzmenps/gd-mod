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

        // the small round button in a corner (see the Config for where)
        auto launcher = collab::CollabLauncher::create();
        this->addChild(launcher, 100);

        collab::Manager::get().setHud(hud, launcher->badge());
        collab::Manager::get().setLauncher(launcher);

        // outlines around what the other players have selected; z -1 keeps it
        // below the editor's own buttons and panels
        if (auto overlay = collab::SelectionOverlay::create()) {
            this->addChild(overlay, -1);
            collab::Manager::get().setOverlay(overlay);
        }

        return true;
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

$on_mod(Loaded) {
    // the shortcut from the Config opens / closes the window
    listenForKeybindSettingPresses("open-key", [](Keybind const&, bool down, bool repeat, double) {
        if (!down || repeat || !collab::Manager::get().inEditor()) return false;
        CollabPopup::toggle();
        return true;
    });

    // moving the button, changing the server or name, ... takes effect right away
    listenForAllSettingChanges([](std::string_view key, std::shared_ptr<SettingV3>) {
        collab::Manager::get().onSettingChanged(key);
    });
}
