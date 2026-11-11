#pragma once

#include <Geode/Geode.hpp>

#include <string>

#include "Status.hpp"

namespace collab {

    // Draws a rounded progress bar into a CCDrawNode (cleared first).
    // The bar occupies x in [0, width] and y in [0, height].
    // progress in [0, 1]; a negative value draws a moving block instead
    // ("working, but no idea how long"), `phase` drives that animation.
    void drawProgressBar(
        cocos2d::CCDrawNode* node, float width, float height,
        float progress, float phase, cocos2d::ccColor3B color
    );

    // Small round status light. Its colour follows the connection status and it
    // pulses while something is in progress.
    class CollabDot : public cocos2d::CCNode {
    protected:
        cocos2d::CCDrawNode* m_draw = nullptr;
        float m_radius = 5.f;
        Status m_status = Status::Offline;
        bool m_pulsing = false;
        bool m_drawn = false;

        bool init(float radius);

    public:
        static CollabDot* create(float radius);
        void setStatus(Status status);
    };

    // Pill at the top of the editor: [dot] ROOM - 3 online, with a thin
    // progress bar underneath while the level is being synced.
    class CollabHud : public cocos2d::CCNode {
    protected:
        geode::NineSlice* m_bg = nullptr;
        CollabDot* m_dot = nullptr;
        cocos2d::CCLabelBMFont* m_label = nullptr;
        cocos2d::CCDrawNode* m_bar = nullptr;

        Status m_status = Status::Offline;
        std::string m_text;
        int m_quantized = -2; // progress in whole percent, -1 = unknown
        float m_phase = 0.f;
        float m_barWidth = 0.f;
        bool m_animating = false;

        bool init();
        void layoutPill();
        void redrawBar();
        void animate(float dt);

    public:
        static CollabHud* create();

        // progress: 0..1 while syncing, negative when unknown / not syncing
        void setState(Status status, std::string const& text, float progress);
    };

}
