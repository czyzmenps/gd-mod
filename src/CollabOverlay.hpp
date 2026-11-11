#pragma once

#include <Geode/Geode.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace collab {

    // What one remote player has selected, ready to be drawn.
    struct OverlayEntry {
        int playerId = 0;
        std::string label;                  // e.g. "Alice - 3 objects"
        cocos2d::ccColor3B color { 255, 255, 255 };
        std::vector<cocos2d::CCRect> boxes; // in the overlay's own coordinate space
    };

    // Draws a coloured outline around every object another player has selected,
    // plus a small name tag. It lives inside the editor UI (below the toolbars),
    // so the outlines keep a constant thickness at any zoom level.
    class SelectionOverlay : public cocos2d::CCNode {
    protected:
        struct LabelSlot {
            cocos2d::CCLabelBMFont* node = nullptr;
            std::string text;
        };

        cocos2d::CCDrawNode* m_draw = nullptr;
        std::unordered_map<int, LabelSlot> m_labels;

        bool init();

    public:
        static SelectionOverlay* create();

        // Replaces everything that is currently shown. An empty list clears the overlay.
        void setEntries(std::vector<OverlayEntry> const& entries);
    };

}
