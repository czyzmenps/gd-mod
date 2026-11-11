#include "CollabLauncher.hpp"

#include "CollabPopup.hpp"
#include "Config.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace geode::prelude;

namespace collab {

    // ------------------------------------------------------------------
    // icon
    // ------------------------------------------------------------------

    CollabIcon* CollabIcon::create(float diameter) {
        auto ret = new CollabIcon();
        if (ret->init(diameter)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool CollabIcon::init(float diameter) {
        if (!CCNode::init()) return false;
        this->setContentSize({ diameter, diameter });
        this->setAnchorPoint({ .5f, .5f });

        // everything is drawn in a 64 x 64 box and scaled to the wanted size
        auto draw = CCDrawNode::create();
        draw->setScale(diameter / 64.f);
        this->addChild(draw);

        ccColor3B const baseColor = { 56, 196, 108 };
        auto const base = toColor4F(baseColor);
        auto const none = ccColor4F { 0.f, 0.f, 0.f, 0.f };

        // round base with a dark rim and a soft highlight
        draw->drawDot(ccp(32.f, 32.f), 32.f, ccColor4F { 0.f, 0.f, 0.f, .6f });
        draw->drawDot(ccp(32.f, 32.f), 30.f, base);
        draw->drawDot(ccp(32.f, 39.f), 22.f, ccColor4F { 1.f, 1.f, 1.f, .13f });

        // a person: round head and shoulders
        auto person = [&](float cx, float headY, float headR, float rx, float ry, float baseY, ccColor4F color) {
            draw->drawDot(ccp(cx, headY), headR, color);
            std::vector<CCPoint> points;
            for (int i = 0; i <= 14; i++) {
                float const angle = 3.14159265f * static_cast<float>(i) / 14.f;
                points.push_back(ccp(cx + rx * std::cos(angle), baseY + ry * std::sin(angle)));
            }
            draw->drawPolygon(points.data(), static_cast<unsigned int>(points.size()), color, 0.f, none);
        };

        // the player in the back
        person(23.f, 40.f, 6.f, 11.f, 14.f, 17.f, ccColor4F { .80f, .94f, .86f, 1.f });
        // the player in front, with a rim in the base colour so the two stay apart
        person(41.f, 42.f, 9.f, 15.5f, 19.f, 13.f, base);
        person(41.f, 42.f, 7.5f, 13.f, 17.f, 14.f, ccColor4F { 1.f, 1.f, 1.f, 1.f });
        return true;
    }

    // ------------------------------------------------------------------
    // launcher
    // ------------------------------------------------------------------

    CollabLauncher* CollabLauncher::create() {
        auto ret = new CollabLauncher();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool CollabLauncher::init() {
        if (!CCNode::init()) return false;

        m_menu = CCMenu::create();
        m_menu->setPosition({ 0.f, 0.f });
        this->addChild(m_menu, 1);

        m_badge = CollabDot::create(4.5f);
        this->addChild(m_badge, 2);

        this->applySettings();
        return true;
    }

    void CollabLauncher::applySettings() {
        std::string const where = cfg::text("button-position");
        bool const hidden = where == "Hidden";
        this->setVisible(!hidden);

        float const scale = std::clamp(static_cast<float>(cfg::number("button-size")), .5f, 1.6f);
        float const diameter = 38.f * scale;

        // a new size means a new button (its tap area has to match the icon)
        if (!m_button || std::abs(diameter - m_builtSize) > .01f) {
            if (m_button) m_button->removeFromParent();
            m_button = CCMenuItemSpriteExtra::create(
                CollabIcon::create(diameter), this, menu_selector(CollabLauncher::onPressed)
            );
            m_menu->addChild(m_button);
            m_builtSize = diameter;
        }
        m_button->setEnabled(!hidden);

        auto const winSize = CCDirector::get()->getWinSize();
        float const margin = 10.f + diameter / 2.f;
        bool const left = where == "Top left" || where == "Bottom left";
        bool const bottom = where == "Bottom left" || where == "Bottom right";
        float const x = left ? margin : winSize.width - margin;
        float const y = bottom ? margin : winSize.height - margin;

        m_button->setPosition({ x, y });
        m_badge->setPosition({ x + diameter * .34f, y + diameter * .34f });
    }

    void CollabLauncher::onPressed(CCObject*) {
        CollabPopup::toggle();
    }

}
