#pragma once

#include <Geode/Geode.hpp>

#include "CollabHud.hpp"

namespace collab {

    // The icon of the mod, drawn in code (two players) on a round green base.
    // Its size is `diameter` pixels, whatever the screen resolution.
    class CollabIcon : public cocos2d::CCNode {
    protected:
        bool init(float diameter);

    public:
        static CollabIcon* create(float diameter);
    };

    // The small round button in a corner of the editor that opens the Collab
    // window. Where it sits and how big it is comes from the mod's Config.
    class CollabLauncher : public cocos2d::CCNode {
    protected:
        cocos2d::CCMenu* m_menu = nullptr;
        CCMenuItemSpriteExtra* m_button = nullptr;
        CollabDot* m_badge = nullptr;
        float m_builtSize = 0.f;

        bool init();
        void onPressed(cocos2d::CCObject*);

    public:
        static CollabLauncher* create();

        // the little status light on the button
        CollabDot* badge() const { return m_badge; }

        // (re)reads position, size and visibility from the Config
        void applySettings();
    };

}
