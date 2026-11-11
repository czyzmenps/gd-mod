#include "CollabOverlay.hpp"

#include "Status.hpp"

#include <algorithm>
#include <unordered_set>

using namespace geode::prelude;

namespace collab {

    SelectionOverlay* SelectionOverlay::create() {
        auto ret = new SelectionOverlay();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool SelectionOverlay::init() {
        if (!CCNode::init()) return false;
        m_draw = CCDrawNode::create();
        this->addChild(m_draw, 0);
        return true;
    }

    void SelectionOverlay::setEntries(std::vector<OverlayEntry> const& entries) {
        m_draw->clear();

        auto const winSize = CCDirector::get()->getWinSize();
        std::unordered_set<int> alive;

        for (auto const& entry : entries) {
            if (entry.boxes.empty()) continue;
            alive.insert(entry.playerId);

            auto const line = toColor4F(entry.color, .95f);
            auto const fill = toColor4F(entry.color, .16f);

            float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
            for (auto const& box : entry.boxes) {
                float const x0 = box.getMinX();
                float const y0 = box.getMinY();
                float const x1 = box.getMaxX();
                float const y1 = box.getMaxY();
                CCPoint verts[4] = { ccp(x0, y0), ccp(x1, y0), ccp(x1, y1), ccp(x0, y1) };
                m_draw->drawPolygon(verts, 4, fill, 1.5f, line);

                minX = std::min(minX, x0);
                minY = std::min(minY, y0);
                maxX = std::max(maxX, x1);
                maxY = std::max(maxY, y1);
            }

            // name tag
            auto& slot = m_labels[entry.playerId];
            if (!slot.node) {
                slot.node = CCLabelBMFont::create("", "chatFont.fnt");
                slot.node->setAnchorPoint({ 0.f, 0.f });
                slot.node->setScale(.55f);
                this->addChild(slot.node, 1);
            }
            if (slot.text != entry.label) {
                slot.text = entry.label;
                slot.node->setString(entry.label.c_str());
            }
            slot.node->setColor(entry.color);

            float const w = slot.node->getContentSize().width * slot.node->getScale();
            float const h = slot.node->getContentSize().height * slot.node->getScale();
            float x = std::clamp(minX, 6.f, std::max(6.f, winSize.width - w - 14.f));
            float y = maxY + 6.f;
            if (y + h + 6.f > winSize.height) y = minY - h - 8.f; // no room above: go below
            y = std::clamp(y, 4.f, std::max(4.f, winSize.height - h - 8.f));

            slot.node->setPosition({ x, y });
            slot.node->setVisible(true);

            // dark pill behind the text (drawn by m_draw, which is below the labels)
            float const px0 = x - 4.f, py0 = y - 2.f, px1 = x + w + 4.f, py1 = y + h + 2.f;
            CCPoint pill[4] = { ccp(px0, py0), ccp(px1, py0), ccp(px1, py1), ccp(px0, py1) };
            m_draw->drawPolygon(pill, 4, ccColor4F { 0.f, 0.f, 0.f, .65f }, 1.f, line);
        }

        // forget name tags of players that are no longer shown
        for (auto it = m_labels.begin(); it != m_labels.end();) {
            if (alive.contains(it->first)) {
                ++it;
                continue;
            }
            if (it->second.node) it->second.node->removeFromParent();
            it = m_labels.erase(it);
        }
    }

}
