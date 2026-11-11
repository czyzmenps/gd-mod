#pragma once

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cctype>
#include <string>

// Everything the player can change in the mod's Config (Geode settings).
namespace collab::cfg {

    // Server that is used when the "Party server" setting is empty. Put the
    // address of your always-on party server here (host or host:port) before
    // building, and nobody ever has to type an address.
    inline constexpr char const* kDefaultServer = "";

    inline constexpr char const* kDefaultPort = "5588";

    inline std::string trimmed(std::string s) {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    }

    inline std::string text(char const* key) {
        return trimmed(geode::Mod::get()->getSettingValue<std::string>(key));
    }

    inline bool flag(char const* key) {
        return geode::Mod::get()->getSettingValue<bool>(key);
    }

    inline double number(char const* key) {
        return geode::Mod::get()->getSettingValue<double>(key);
    }

    // The party server: Config -> (value saved by older versions) -> built-in default.
    inline std::string server() {
        std::string s = text("server");
        if (s.empty()) s = trimmed(geode::Mod::get()->getSavedValue<std::string>("server", ""));
        if (s.empty()) s = trimmed(kDefaultServer);
        return s;
    }

    // The player's name: Config -> Geometry Dash account name -> "Player".
    inline std::string playerName() {
        std::string name = text("player-name");
        if (name.empty()) {
            if (auto* accounts = GJAccountManager::get()) {
                name = trimmed(std::string(accounts->m_username));
            }
        }
        if (name.empty()) name = trimmed(geode::Mod::get()->getSavedValue<std::string>("name", ""));
        if (name.empty()) name = "Player";
        if (name.size() > 20) name.resize(20);
        return name;
    }

}
