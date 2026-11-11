#include "CollabHud.hpp"

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace collab {

    // ------------------------------------------------------------------
    // progress bar
    // ------------------------------------------------------------------

    void drawProgressBar(
        CCDrawNode* node, float width, float height,
        float progress, float phase, ccColor3B color
    ) {
        node->clear();
        float const r = height / 2.f;
        float const x0 = r;
        float const x1 = std::max(width - r, x0 + 1.f);

        // track
        node->drawSegment(ccp(x0, r), ccp(x1, r), r, ccColor4F { 0.f, 0.f, 0.f, .5f });

        auto const fill = toColor4F(color);
        float const inner = r * .75f;

        if (progress >= 0.f) {
            progress = std::min(progress, 1.f);
            float const end = x0 + (x1 - x0) * progress;
            if (end - x0 < 1.f) node->drawDot(ccp(x0, r), inner, fill);
            else node->drawSegment(ccp(x0, r), ccp(end, r), inner, fill);
        }
        else {
            // a block bouncing from side to side
            float const span = x1 - x0;
            float const len = span * .3f;
            float const t = phase - std::floor(phase);
            float const tri = t < .5f ? t * 2.f : (1.f - t) * 2.f;
            float const start = x0 + (span - len) * tri;
            node->drawSegment(ccp(start, r), ccp(start + len, r), inner, fill);
        }
    }

    // ------------------------------------------------------------------
    // status dot
    // ------------------------------------------------------------------

    CollabDot* CollabDot::create(float radius) {
        auto ret = new CollabDot();
        if (ret->init(radius)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool CollabDot::init(float radius) {
        if (!CCNode::init()) return false;
        m_radius = radius;
        this->setContentSize({ radius * 2.f, radius * 2.f });
        this->setAnchorPoint({ .5f, .5f });

        m_draw = CCDrawNode::create();
        this->addChild(m_draw);
        this->setStatus(Status::Offline);
        return true;
    }

    void CollabDot::setStatus(Status status) {
        if (m_drawn && status == m_status) return;
        m_drawn = true;
        m_status = status;

        float const r = m_radius;
        m_draw->clear();
        m_draw->drawDot(ccp(r, r), r + 1.2f, ccColor4F { 0.f, 0.f, 0.f, .55f });
        m_draw->drawDot(ccp(r, r), r, toColor4F(statusColor(status)));
        m_draw->drawDot(ccp(r - r * .25f, r + r * .3f), r * .35f, ccColor4F { 1.f, 1.f, 1.f, .35f });

        bool const busy = statusBusy(status);
        if (busy && !m_pulsing) {
            m_pulsing = true;
            this->runAction(CCRepeatForever::create(CCSequence::create(
                CCScaleTo::create(.5f, 1.3f),
                CCScaleTo::create(.5f, .9f),
                nullptr
            )));
        }
        else if (!busy && m_pulsing) {
            m_pulsing = false;
            this->stopAllActions();
            this->setScale(1.f);
        }
    }

    // ------------------------------------------------------------------
    // HUD pill
    // ------------------------------------------------------------------

    namespace {
        constexpr float kPillHeight = 24.f;
        constexpr float kPillPad = 10.f;
        constexpr float kDotRadius = 4.5f;
        constexpr float kBarHeight = 4.f;
        constexpr float kLabelScale = .5f;
    }

    CollabHud* CollabHud::create() {
        auto ret = new CollabHud();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool CollabHud::init() {
        if (!CCNode::init()) return false;
        this->setAnchorPoint({ .5f, 1.f });

        m_bg = NineSlice::create("square02b_001.png", { 0.f, 0.f, 80.f, 80.f });
        m_bg->setScale(.5f);
        m_bg->setAnchorPoint({ 0.f, 0.f });
        m_bg->setPosition({ 0.f, 0.f });
        m_bg->setColor({ 0, 0, 0 });
        m_bg->setOpacity(130);
        this->addChild(m_bg, 0);

        m_bar = CCDrawNode::create();
        this->addChild(m_bar, 1);

        m_dot = CollabDot::create(kDotRadius);
        this->addChild(m_dot, 2);

        m_label = CCLabelBMFont::create("", "chatFont.fnt");
        m_label->setScale(kLabelScale);
        m_label->setAnchorPoint({ 0.f, .5f });
        this->addChild(m_label, 2);

        this->layoutPill();
        this->setVisible(false);
        return true;
    }

    void CollabHud::layoutPill() {
        bool const bar = m_status == Status::Syncing;
        float const labelW = m_label->getContentSize().width * m_label->getScale();
        float const width = std::max(90.f, kPillPad * 2.f + kDotRadius * 2.f + 6.f + labelW);
        float const textY = kPillHeight / 2.f + (bar ? 2.f : 0.f);

        this->setContentSize({ width, kPillHeight });
        m_bg->setContentSize({ width * 2.f, kPillHeight * 2.f });
        m_dot->setPosition({ kPillPad + kDotRadius, textY });
        m_label->setPosition({ kPillPad + kDotRadius * 2.f + 6.f, textY });

        m_barWidth = width - kPillPad * 2.f;
        m_bar->setPosition({ kPillPad, 3.f });
        m_bar->setVisible(bar);
    }

    void CollabHud::redrawBar() {
        if (m_status != Status::Syncing) return;
        float const progress = m_quantized < 0 ? -1.f : m_quantized / 100.f;
        drawProgressBar(m_bar, m_barWidth, kBarHeight, progress, m_phase, statusColor(m_status));
    }

    void CollabHud::animate(float dt) {
        m_phase += dt * .7f;
        this->redrawBar();
    }

    void CollabHud::setState(Status status, std::string const& text, float progress) {
        this->setVisible(status != Status::Offline);

        int const quantized = progress < 0.f ? -1 : static_cast<int>(std::min(progress, 1.f) * 100.f);
        if (status == m_status && text == m_text && quantized == m_quantized) return;

        bool const statusChanged = status != m_status;
        bool const textChanged = text != m_text;
        m_status = status;
        m_quantized = quantized;

        m_dot->setStatus(status);
        if (textChanged) {
            m_text = text;
            m_label->setString(text.c_str());
        }
        if (statusChanged || textChanged) this->layoutPill();

        // an unknown progress means "keep moving" - needs a per-frame redraw
        bool const wantAnimation = status == Status::Syncing && quantized < 0;
        if (wantAnimation && !m_animating) {
            m_animating = true;
            this->schedule(schedule_selector(CollabHud::animate));
        }
        else if (!wantAnimation && m_animating) {
            m_animating = false;
            this->unschedule(schedule_selector(CollabHud::animate));
        }
        this->redrawBar();
    }

}
