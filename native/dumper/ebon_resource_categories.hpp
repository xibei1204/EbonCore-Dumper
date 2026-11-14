#pragma once

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <vector>

namespace ebon::resources {

inline const std::vector<std::string>& categoryNames() {
    static const std::vector<std::string> names = {
        "characters", "gacha", "activities_systems", "story_quests",
        "localization", "character_skins", "maps", "scripts",
        "items_economy", "combat_ai", "protocols", "blueprints", "other_assets"
    };
    return names;
}

namespace category_detail {

// Delimiter boundaries depend only on the lowered spelling, so changing the
// casing of a supplied path cannot change its categories.
inline std::vector<std::string> words(const std::string& identity) {
    std::vector<std::string> result;
    std::string current;
    for (std::size_t i = 0; i < identity.size(); ++i) {
        const auto c = static_cast<unsigned char>(identity[i]);
        if (!std::isalnum(c)) {
            if (!current.empty()) { result.push_back(current); current.clear(); }
            continue;
        }
        current += static_cast<char>(std::tolower(c));
    }
    if (!current.empty()) result.push_back(current);
    return result;
}

inline bool anyWord(const std::vector<std::string>& words,
                    std::initializer_list<const char*> candidates) {
    for (const auto* candidate : candidates)
        if (std::find(words.begin(), words.end(), candidate) != words.end()) return true;
    return false;
}

inline bool contains(const std::string& identity,
                     std::initializer_list<const char*> candidates) {
    for (const auto* candidate : candidates)
        if (identity.find(candidate) != std::string::npos) return true;
    return false;
}

inline bool hasSkin(const std::string& identity) {
    for (auto at = identity.find("skin"); at != std::string::npos;
         at = identity.find("skin", at + 4)) {
        // Skeletal rendering terminology does not describe a cosmetic skin.
        const auto suffix = identity.substr(at);
        if (suffix.compare(0, 7, "skinned") != 0 &&
            suffix.compare(0, 8, "skinning") != 0 &&
            suffix.compare(0, 10, "skinweight") != 0 &&
            suffix.compare(0, 9, "skincache") != 0) return true;
    }
    return false;
}

} // namespace category_detail

inline bool scriptIdentity(const std::string& identity) {
    auto lower = identity;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return category_detail::anyWord(category_detail::words(identity), {"lua", "luac", "luajit", "scripts", "scriptasset"}) ||
        category_detail::contains(lower, {"unlua", "slua", "luaasset", "luascript", "luabytecode"});
}

inline std::vector<std::string> categories(const std::string& identity) {
    const auto words = category_detail::words(identity);
    auto lower = identity;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const bool character = category_detail::anyWord(words, {
        "role", "roles", "hero", "heroes", "avatar", "avatars", "skill",
        "skills", "weapon", "weapons", "equipment", "equipments", "talent",
        "talents", "attribute", "attributes", "growth", "cultivation"
    }) || category_detail::contains(lower, {
        "character", "rolelevel", "roleexp", "roleattribute", "roleupgrade",
        "roledata", "roleconfig", "rolemanager", "rolequality", "rolebreak",
        "breakthrough", "awaken", "upgrade", "equipment", "skill", "weapon",
        "attribute", "talent", "cultivate", "cultivation"
    });
    const bool gacha = category_detail::contains(lower, {
        "gacha", "gashapon", "lottery", "monopoly", "cardpool", "drawpool",
        "summonpool", "poolguarantee", "poolprobability"
    });
    const bool systems = category_detail::anyWord(words, {
        "system", "systems", "setting", "settings", "config", "global",
        "activity", "activities", "event", "events", "shop", "store",
        "checkin", "signin", "battlepass", "unlock", "platform"
    }) || category_detail::contains(lower, {
        "activity", "opact", "signin", "checkin", "newbie", "returningplayer",
        "battlepass", "monthcard", "shopgoods", "systemconfig", "globalconfig",
        "settings", "systematic"
    });
    const bool story = category_detail::contains(lower, {
        "quest", "mission", "story", "narrative", "dialog", "subtitle",
        "cutscene", "cinematic", "conversation", "characterfile"
    }) || category_detail::anyWord(words, {"plot", "talk"});
    const bool localization = category_detail::contains(lower, {
        "localization", "localisation", "stringtable", "texttable", "richtext",
        "dialog", "subtitle", "translation", "localized", "localised",
        "textdata", "textresource", "textblock", "textid", "textcontent",
        "textstyle", "textname", "textdesc", "displaytext"
    }) || category_detail::anyWord(words, {
        "text", "texts", "language", "languages", "voice", "talk", "st"
    });
    const bool skins = category_detail::hasSkin(lower) ||
        category_detail::contains(lower, {
            "fashion", "appearance", "costume", "outfit", "decoration",
            "dye"
        });

    const bool maps = category_detail::anyWord(words, {"map", "maps", "level", "levels", "world", "umap", "spawn", "spawns"}) ||
        category_detail::contains(lower, {"worldpartition", "worlddata", "levelstream", "scenecomponent", "spawnpoint", "teleport", "navigation", "navmesh", "waypoint", "persistentlevel", "runtimeactor", "runtimecomponent"});
    const bool scripts = scriptIdentity(identity);
    const bool economy = category_detail::anyWord(words, {"item", "items", "inventory", "reward", "rewards", "drop", "drops", "currency", "shop", "goods"}) ||
        category_detail::contains(lower, {"itemconfig", "itemdata", "reward", "droptable", "inventory", "currency", "shopgoods", "loottable"});
    const bool combat = category_detail::anyWord(words, {"combat", "battle", "monster", "npc", "damage", "buff", "ai"}) ||
        category_detail::contains(lower, {"behaviortree", "blackboard", "ability", "gameplayeffect", "damage", "monster", "enemy", "aicontroller"});
    const bool protocols = category_detail::anyWord(words, {"proto", "protobuf", "protocol", "rpc", "network", "net", "message", "opcode"}) ||
        category_detail::contains(lower, {"protocol", "netconfig", "rpcconfig", "packet"});
    const bool blueprints = category_detail::contains(lower, {"blueprint", "statetree"});
    const bool selected[] = {character, gacha, systems, story, localization, skins,
                            maps, scripts, economy, combat, protocols, blueprints, false};
    std::vector<std::string> result;
    const auto& names = categoryNames();
    for (std::size_t i = 0; i < names.size(); ++i)
        if (selected[i]) result.push_back(names[i]);
    if (result.empty()) result.push_back("other_assets");
    return result;
}

} // namespace ebon::resources
