#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// One macro input, ported from the XDBot GDR format (src/gdr/gdr.hpp).
struct RGInput {
    uint32_t frame = 0; // frame index at `RGMacro::framerate`
    int button = 1;
    bool player2 = false;
    bool down = false; // true = press / hold start, false = release
};

struct RGMacro {
    bool loaded = false;
    float framerate = 240.f;
    float duration = 0.f;
    std::string author;
    std::string botName;
    std::string botVersion;
    std::string levelName;
    std::string error;
    std::vector<RGInput> inputs; // sorted by frame
};

// Loads a .gdr (msgpack) or .gdr.json (plain JSON) macro.
// On failure returns RGMacro with loaded = false and `error` set.
RGMacro loadGDRMacro(std::filesystem::path const& path);
