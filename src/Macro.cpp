#include "Macro.hpp"

#include <algorithm>
#include <sstream>

#include <Geode/Geode.hpp>
#include <Geode/utils/file.hpp>

#include "thirdparty/json.hpp"

using namespace geode::prelude;

namespace {

std::vector<std::string> splitByChar(std::string const& str, char splitChar) {
    std::vector<std::string> strs;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, splitChar)) {
        strs.push_back(item);
    }
    return strs;
}

// Simple semantic version compare (missing components count as 0).
int compareVersions(std::vector<std::string> const& a, std::vector<std::string> const& b) {
    auto numAt = [](std::vector<std::string> const& v, size_t i) -> long {
        if (i >= v.size()) return 0;
        try {
            return std::stol(v[i]);
        } catch (...) {
            return 0;
        }
    };
    for (size_t i = 0; i < 3; i++) {
        long av = numAt(a, i);
        long bv = numAt(b, i);
        if (av != bv) return av < bv ? -1 : 1;
    }
    return 0;
}

// Replicates the frame offset logic from XDBot's gdr.hpp:
// xdBot macros recorded before version 2.3.6 are shifted by +1 frame.
int xdBotFrameOffset(std::string botName, std::string botVersion) {
    if (botName != "xdBot") return 0;
    if (botVersion.empty()) return 1;
    std::string ver = botVersion;
    if (ver.front() == 'v') ver = ver.substr(1);
    auto splitVer = splitByChar(ver, '.');
    if (splitVer.size() > 3) return 1;
    // macroVer >= 2.3.6 -> no offset
    if (compareVersions(splitVer, {"2", "3", "6"}) >= 0) return 0;
    return 1;
}

} // namespace

RGMacro loadGDRMacro(std::filesystem::path const& path) {
    RGMacro macro;

    auto dataRes = geode::utils::file::readBinary(path);
    if (!dataRes) {
        macro.error = dataRes.unwrapErr();
        return macro;
    }
    auto& data = dataRes.unwrap();
    if (data.empty()) {
        macro.error = "File is empty";
        return macro;
    }

    using nlohmann::json;
    json replayJson = json::from_msgpack(data, true, false);
    if (replayJson.is_discarded()) {
        replayJson = json::parse(data, nullptr, false);
        if (replayJson.is_discarded()) {
            macro.error = "Not a valid .gdr / .gdr.json file";
            return macro;
        }
    }

    try {
        if (replayJson.contains("framerate") && replayJson["framerate"].is_number()) {
            macro.framerate = replayJson["framerate"].get<float>();
        }
        if (macro.framerate <= 0.f) macro.framerate = 240.f;

        if (replayJson.contains("duration") && replayJson["duration"].is_number()) {
            macro.duration = replayJson["duration"].get<float>();
        }
        if (replayJson.contains("author") && replayJson["author"].is_string()) {
            macro.author = replayJson["author"].get<std::string>();
        }
        if (replayJson.contains("bot") && replayJson["bot"].is_object()) {
            auto& bot = replayJson["bot"];
            if (bot.contains("name") && bot["name"].is_string())
                macro.botName = bot["name"].get<std::string>();
            if (bot.contains("version") && bot["version"].is_string())
                macro.botVersion = bot["version"].get<std::string>();
        }
        if (replayJson.contains("level") && replayJson["level"].is_object()) {
            auto& level = replayJson["level"];
            if (level.contains("name") && level["name"].is_string())
                macro.levelName = level["name"].get<std::string>();
        }

        int offset = xdBotFrameOffset(macro.botName, macro.botVersion);

        if (!replayJson.contains("inputs") || !replayJson["inputs"].is_array()) {
            macro.error = "Macro has no inputs";
            return macro;
        }

        for (auto const& inputJson : replayJson["inputs"]) {
            if (!inputJson.is_object()) continue;
            if (!inputJson.contains("frame") || inputJson["frame"].is_null()) continue;

            RGInput input;
            input.frame = static_cast<uint32_t>(inputJson["frame"].get<int64_t>() + offset);
            input.button = inputJson.value("btn", 1);
            input.player2 = inputJson.value("2p", false);
            input.down = inputJson.value("down", false);
            macro.inputs.push_back(input);
        }

        std::sort(macro.inputs.begin(), macro.inputs.end(),
                  [](RGInput const& a, RGInput const& b) { return a.frame < b.frame; });

        macro.loaded = true;
    } catch (std::exception const& e) {
        macro.error = e.what();
        macro.loaded = false;
        macro.inputs.clear();
    }

    return macro;
}
