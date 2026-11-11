#pragma once

#include <Geode/Geode.hpp>

namespace collab {

    enum class Status {
        Offline,    // not connected
        Connecting, // TCP connect in progress
        Joining,    // connected, waiting for the server to accept us
        Confirm,    // waiting for the player to confirm replacing their level
        Syncing,    // uploading / downloading the level
        Live        // fully synced, changes flow both ways
    };

    // Colour used for the status dot everywhere in the UI.
    inline cocos2d::ccColor3B statusColor(Status status) {
        switch (status) {
            case Status::Live: return { 90, 235, 110 };
            case Status::Syncing: return { 90, 190, 255 };
            case Status::Offline: return { 150, 150, 150 };
            default: return { 255, 200, 70 }; // connecting / joining / waiting
        }
    }

    // True while something is in progress (the dot pulses).
    inline bool statusBusy(Status status) {
        return status != Status::Offline && status != Status::Live;
    }

    inline cocos2d::ccColor4F toColor4F(cocos2d::ccColor3B c, float alpha = 1.f) {
        return { c.r / 255.f, c.g / 255.f, c.b / 255.f, alpha };
    }

}
