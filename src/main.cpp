#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace {
    using Clock = std::chrono::steady_clock;

    struct MacroInput {
        double time = 0.0;
        int frame = 0;
        bool down = true;
        int player = 1;
        int button = 1;
        bool judged = false;
    };

    struct Settings {
        bool enabled;
        std::string macroPath;
        bool reloadMacroOnReset;
        double macroFps;

        int inputWindowMs;
        int inWindowMs;
        bool showMsFeedback;
        bool showInText;
        bool strictPlayerMatch;
        bool strictButtonMatch;
        bool gdBoolTrueIsP1;
        bool cbfSubframeClock;

        int lookaheadMs;
        int pastVisibleMs;
        float lineWidth;
        float lineHeightPercent;
        float lineAlpha;
        bool glowEnabled;
        float glowAlpha;
        std::string lineColor;
        std::string accentColor;
        bool showEventLabels;
        float markerDotSize;
        float feedbackFontScale;
        float feedbackYOffset;
        int feedbackDurationMs;
        bool attachIconUntilRelease;

        bool disableInShip;
        bool disableInWave;
        bool hideFeedbackInShipWave;
        bool clearLinesOnShipWave;

        float fallbackScrollPps;
        int maxVisibleLines;
        bool debugLogParser;
    };

    template <class T>
    T setting(char const* key) {
        return Mod::get()->getSettingValue<T>(key);
    }

    Settings getSettings() {
        return Settings {
            setting<bool>("enabled"),
            setting<std::string>("macro-path"),
            setting<bool>("reload-macro-on-reset"),
            static_cast<double>(setting<double>("macro-fps")),

            setting<int64_t>("input-window-ms") > 0 ? static_cast<int>(setting<int64_t>("input-window-ms")) : 120,
            setting<int64_t>("in-window-ms") > 0 ? static_cast<int>(setting<int64_t>("in-window-ms")) : 18,
            setting<bool>("show-ms-feedback"),
            setting<bool>("show-in-text"),
            setting<bool>("strict-player-match"),
            setting<bool>("strict-button-match"),
            setting<bool>("gd-bool-true-is-p1"),
            setting<bool>("cbf-subframe-clock"),

            static_cast<int>(setting<int64_t>("lookahead-ms")),
            static_cast<int>(setting<int64_t>("past-visible-ms")),
            static_cast<float>(setting<double>("line-width")),
            static_cast<float>(setting<double>("line-height-percent")),
            static_cast<float>(setting<double>("line-alpha")),
            setting<bool>("glow-enabled"),
            static_cast<float>(setting<double>("glow-alpha")),
            setting<std::string>("line-color"),
            setting<std::string>("accent-color"),
            setting<bool>("show-event-labels"),
            static_cast<float>(setting<double>("marker-dot-size")),
            static_cast<float>(setting<double>("feedback-font-scale")),
            static_cast<float>(setting<double>("feedback-y-offset")),
            static_cast<int>(setting<int64_t>("feedback-duration-ms")),
            setting<bool>("attach-icon-until-release"),

            setting<bool>("disable-in-ship"),
            setting<bool>("disable-in-wave"),
            setting<bool>("hide-feedback-in-ship-wave"),
            setting<bool>("clear-lines-on-ship-wave"),

            static_cast<float>(setting<double>("fallback-scroll-pps")),
            static_cast<int>(setting<int64_t>("max-visible-lines")),
            setting<bool>("debug-log-parser")
        };
    }

    ccColor4F parseHexColor(std::string hex, float alpha) {
        if (!hex.empty() && hex[0] == '#') {
            hex.erase(hex.begin());
        }

        if (hex.size() != 6) {
            return ccc4f(1.f, 1.f, 1.f, alpha);
        }

        auto parseByte = [](std::string const& s) -> int {
            try {
                return std::stoi(s, nullptr, 16);
            } catch (...) {
                return 255;
            }
        };

        int r = parseByte(hex.substr(0, 2));
        int g = parseByte(hex.substr(2, 2));
        int b = parseByte(hex.substr(4, 2));

        return ccc4f(
            std::clamp(r / 255.f, 0.f, 1.f),
            std::clamp(g / 255.f, 0.f, 1.f),
            std::clamp(b / 255.f, 0.f, 1.f),
            std::clamp(alpha, 0.f, 1.f)
        );
    }

    std::string readFileText(std::filesystem::path const& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return {};

        std::ostringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    std::vector<uint8_t> readFileBytes(std::filesystem::path const& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return {};

        file.seekg(0, std::ios::end);
        auto size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> data(static_cast<size_t>(size));
        if (!data.empty()) {
            file.read(reinterpret_cast<char*>(data.data()), size);
        }

        return data;
    }

    bool mostlyPrintable(std::vector<uint8_t> const& data) {
        if (data.empty()) return false;

        size_t printable = 0;
        for (auto b : data) {
            if (b == '\n' || b == '\r' || b == '\t' || (b >= 32 && b < 127)) {
                printable++;
            }
        }

        return printable > data.size() * 8 / 10;
    }

    std::optional<double> extractNumber(std::string const& obj, std::vector<std::string> const& keys) {
        for (auto const& key : keys) {
            std::regex re("\"" + key + "\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)", std::regex::icase);
            std::smatch m;
            if (std::regex_search(obj, m, re)) {
                try {
                    return std::stod(m[1].str());
                } catch (...) {}
            }
        }
        return std::nullopt;
    }

    std::optional<bool> extractBool(std::string const& obj, std::vector<std::string> const& keys) {
        for (auto const& key : keys) {
            std::regex re("\"" + key + "\"\\s*:\\s*(true|false|0|1)", std::regex::icase);
            std::smatch m;
            if (std::regex_search(obj, m, re)) {
                auto v = m[1].str();
                std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                return v == "true" || v == "1";
            }
        }
        return std::nullopt;
    }

    std::vector<MacroInput> parseGdrJson(std::string const& text, double fps, bool debug) {
        std::vector<MacroInput> result;

        std::regex objectRe("\\{[^\\{\\}]*\\}");
        auto begin = std::sregex_iterator(text.begin(), text.end(), objectRe);
        auto end = std::sregex_iterator();

        for (auto it = begin; it != end; ++it) {
            auto obj = it->str();

            MacroInput input;

            auto frame = extractNumber(obj, {"frame", "f", "fr"});
            auto time = extractNumber(obj, {"time", "seconds", "sec"});
            auto ms = extractNumber(obj, {"ms", "millis", "timestampMs", "timestamp_ms"});

            if (time) {
                input.time = *time;
                input.frame = static_cast<int>(std::round(input.time * fps));
            } else if (ms) {
                input.time = *ms / 1000.0;
                input.frame = static_cast<int>(std::round(input.time * fps));
            } else if (frame) {
                input.frame = static_cast<int>(*frame);
                input.time = input.frame / fps;
            } else {
                continue;
            }

            auto down = extractBool(obj, {"down", "pressed", "press", "push", "hold", "isDown", "is_down"});
            input.down = down.value_or(true);

            auto release = extractBool(obj, {"release", "up"});
            if (release.value_or(false)) {
                input.down = false;
            }

            auto player = extractNumber(obj, {"player", "p"});
            input.player = player ? std::clamp(static_cast<int>(*player), 1, 2) : 1;

            auto player2 = extractBool(obj, {"player2", "p2", "isPlayer2", "is_player_2"});
            if (player2.value_or(false)) {
                input.player = 2;
            }

            auto button = extractNumber(obj, {"button", "btn", "key", "state"});
            input.button = button ? std::max(0, static_cast<int>(*button)) : 1;

            result.push_back(input);
        }

        if (debug) {
            log::info("RhythmGuide: parsed {} json inputs", result.size());
        }

        return result;
    }

    std::vector<MacroInput> parseTextGdr(std::string const& text, double fps, bool debug) {
        std::vector<MacroInput> result;
        std::istringstream ss(text);
        std::string line;

        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            if (line[0] == '#' || line[0] == ';') continue;

            for (auto& c : line) {
                if (c == ',' || c == ';' || c == '|') c = ' ';
            }

            std::istringstream ls(line);

            double first = 0.0;
            int down = 1;
            int player = 1;
            int button = 1;

            if (!(ls >> first)) continue;
            ls >> down;
            ls >> player;
            ls >> button;

            MacroInput input;

            // If the first value is huge, treat as frame.
            // If it has decimal point in original line, treat as seconds.
            bool hasDecimal = line.find('.') != std::string::npos;
            if (hasDecimal) {
                input.time = first;
                input.frame = static_cast<int>(std::round(first * fps));
            } else {
                input.frame = static_cast<int>(first);
                input.time = input.frame / fps;
            }

            input.down = down != 0;
            input.player = std::clamp(player, 1, 2);
            input.button = std::max(0, button);

            result.push_back(input);
        }

        if (debug) {
            log::info("RhythmGuide: parsed {} text gdr inputs", result.size());
        }

        return result;
    }

    template <class T>
    T readLE(std::vector<uint8_t> const& data, size_t off) {
        T out {};
        if (off + sizeof(T) > data.size()) return out;
        std::memcpy(&out, data.data() + off, sizeof(T));
        return out;
    }

    std::vector<MacroInput> parseBinaryGdrFallback(std::vector<uint8_t> const& data, double fps, bool debug) {
        std::vector<MacroInput> best;

        // Fallback parser for simple XDBot-like packed records.
        // Supported guessed layouts:
        // 8 bytes:  u32 frame, u8 down, u8 player, u8 button, u8 reserved
        // 12 bytes: u32 frame, u32 button, u8 down, u8 player, padding
        // 16 bytes: double time, u8 down, u8 player, u8 button, padding
        std::vector<size_t> offsetsToTry = {0, 4, 8, 12, 16, 32, 64};

        for (auto offset : offsetsToTry) {
            if (offset >= data.size()) continue;

            {
                std::vector<MacroInput> tmp;
                for (size_t pos = offset; pos + 8 <= data.size(); pos += 8) {
                    uint32_t frame = readLE<uint32_t>(data, pos);
                    uint8_t down = data[pos + 4];
                    uint8_t player = data[pos + 5];
                    uint8_t button = data[pos + 6];

                    if (frame > 100000000) {
                        tmp.clear();
                        break;
                    }

                    MacroInput input;
                    input.frame = static_cast<int>(frame);
                    input.time = input.frame / fps;
                    input.down = down != 0;
                    input.player = std::clamp<int>(player == 0 ? 1 : player, 1, 2);
                    input.button = button;
                    tmp.push_back(input);
                }

                if (tmp.size() > best.size()) best = tmp;
            }

            {
                std::vector<MacroInput> tmp;
                for (size_t pos = offset; pos + 16 <= data.size(); pos += 16) {
                    double time = readLE<double>(data, pos);
                    uint8_t down = data[pos + 8];
                    uint8_t player = data[pos + 9];
                    uint8_t button = data[pos + 10];

                    if (!std::isfinite(time) || time < 0.0 || time > 36000.0) {
                        tmp.clear();
                        break;
                    }

                    MacroInput input;
                    input.time = time;
                    input.frame = static_cast<int>(std::round(time * fps));
                    input.down = down != 0;
                    input.player = std::clamp<int>(player == 0 ? 1 : player, 1, 2);
                    input.button = button;
                    tmp.push_back(input);
                }

                if (tmp.size() > best.size()) best = tmp;
            }
        }

        if (debug) {
            log::info("RhythmGuide: parsed {} binary fallback gdr inputs", best.size());
        }

        return best;
    }

    std::vector<MacroInput> loadMacro(Settings const& s) {
        std::vector<MacroInput> result;

        if (s.macroPath.empty()) {
            if (s.debugLogParser) {
                log::warn("RhythmGuide: macro path is empty");
            }
            return result;
        }

        std::filesystem::path path = s.macroPath;
        if (!std::filesystem::exists(path)) {
            log::warn("RhythmGuide: macro file does not exist: {}", s.macroPath);
            return result;
        }

        auto ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (ext == ".json" || path.filename().string().find(".gdr.json") != std::string::npos) {
            auto text = readFileText(path);
            result = parseGdrJson(text, s.macroFps, s.debugLogParser);
        } else {
            auto bytes = readFileBytes(path);
            if (mostlyPrintable(bytes)) {
                std::string text(bytes.begin(), bytes.end());

                if (text.find('{') != std::string::npos && text.find('}') != std::string::npos) {
                    result = parseGdrJson(text, s.macroFps, s.debugLogParser);
                } else {
                    result = parseTextGdr(text, s.macroFps, s.debugLogParser);
                }
            } else {
                result = parseBinaryGdrFallback(bytes, s.macroFps, s.debugLogParser);
            }
        }

        std::sort(result.begin(), result.end(), [](auto const& a, auto const& b) {
            return a.time < b.time;
        });

        if (s.debugLogParser) {
            log::info("RhythmGuide: loaded {} inputs from {}", result.size(), s.macroPath);
        }

        return result;
    }

    class RhythmGuideLayer final : public CCLayer {
    protected:
        PlayLayer* m_playLayer = nullptr;
        Settings m_settings;
        std::vector<MacroInput> m_inputs;

        double m_elapsed = 0.0;
        float m_lastDt = 1.f / 60.f;
        Clock::time_point m_lastUpdateWall = Clock::now();

        float m_lastPlayerX = 0.f;
        float m_estimatedPps = 360.f;

        bool m_wasSuppressed = false;

        struct ActiveLine {
            size_t inputIndex = 0;
            CCNode* node = nullptr;
        };

        std::vector<ActiveLine> m_activeLines;
        size_t m_scanIndex = 0;

        CCNode* m_feedbackRoot = nullptr;
        CCLabelBMFont* m_feedbackLabel = nullptr;
        CCDrawNode* m_feedbackIcon = nullptr;
        double m_feedbackUntil = 0.0;
        int m_activeHeldInputs = 0;

    public:
        static RhythmGuideLayer* create(PlayLayer* playLayer) {
            auto ret = new RhythmGuideLayer();
            if (ret && ret->init(playLayer)) {
                ret->autorelease();
                return ret;
            }

            CC_SAFE_DELETE(ret);
            return nullptr;
        }

        bool init(PlayLayer* playLayer) {
            if (!CCLayer::init()) return false;

            m_playLayer = playLayer;
            m_settings = getSettings();
            m_inputs = loadMacro(m_settings);

            setTouchEnabled(false);
            setMouseEnabled(false);
            setKeyboardEnabled(false);

            auto player = getPlayer();
            if (player) {
                m_lastPlayerX = player->getPositionX();
            }

            createFeedbackNode();

            scheduleUpdate();

            return true;
        }

        void resetGuide(bool reloadMacro) {
            removeAllLines();

            m_settings = getSettings();

            if (reloadMacro) {
                m_inputs = loadMacro(m_settings);
            }

            for (auto& input : m_inputs) {
                input.judged = false;
            }

            m_elapsed = 0.0;
            m_scanIndex = 0;
            m_feedbackUntil = 0.0;
            m_activeHeldInputs = 0;
            m_wasSuppressed = false;

            auto player = getPlayer();
            if (player) {
                m_lastPlayerX = player->getPositionX();
            }

            if (m_feedbackRoot) {
                m_feedbackRoot->setVisible(false);
            }

            m_lastUpdateWall = Clock::now();
        }

        void update(float dt) override {
            CCLayer::update(dt);

            if (!m_settings.enabled) {
                removeAllLines();
                if (m_feedbackRoot) m_feedbackRoot->setVisible(false);
                return;
            }

            m_lastDt = std::max(dt, 0.0001f);
            m_elapsed += dt;
            m_lastUpdateWall = Clock::now();

            updateVelocity(dt);

            bool suppressed = isSuppressedByGameMode();

            if (suppressed && !m_wasSuppressed && m_settings.clearLinesOnShipWave) {
                removeAllLines();
            }

            m_wasSuppressed = suppressed;

            if (!suppressed) {
                updateLines();
            } else {
                removeAllLines();
            }

            updateFeedbackPosition();
        }

        void handleInput(bool down, bool gdPlayerBool, int button) {
            if (!m_settings.enabled) return;

            int player = 1;
            if (m_settings.gdBoolTrueIsP1) {
                player = gdPlayerBool ? 1 : 2;
            } else {
                player = gdPlayerBool ? 2 : 1;
            }

            double now = currentInputTime();

            if (down) {
                m_activeHeldInputs++;
            } else {
                m_activeHeldInputs = std::max(0, m_activeHeldInputs - 1);
            }

            auto match = findBestInput(now, down, player, button);

            if (match && m_settings.showMsFeedback) {
                double deltaMs = (now - m_inputs[*match].time) * 1000.0;
                showFeedback(deltaMs);
                m_inputs[*match].judged = true;
            } else if (m_settings.showMsFeedback) {
                showRawFeedback("—");
            }

            if (!down && m_settings.attachIconUntilRelease && m_activeHeldInputs <= 0) {
                m_feedbackUntil = std::min(m_feedbackUntil, m_elapsed + 0.12);
            }
        }

    private:
        PlayerObject* getPlayer() const {
            if (!m_playLayer) return nullptr;
            return m_playLayer->m_player1;
        }

        bool isSuppressedByGameMode() const {
            auto player = getPlayer();
            if (!player) return false;

            bool ship = false;
            bool wave = false;

            // Geode bindings for GD 2.2 usually expose these fields.
            ship = player->m_isShip;
            wave = player->m_isDart;

            if (ship && m_settings.disableInShip) return true;
            if (wave && m_settings.disableInWave) return true;

            return false;
        }

        double currentInputTime() const {
            if (!m_settings.cbfSubframeClock) {
                return m_elapsed;
            }

            auto now = Clock::now();
            double wallDelta = std::chrono::duration<double>(now - m_lastUpdateWall).count();

            // Clamp avoids huge values after pause/window drag.
            wallDelta = std::clamp(wallDelta, 0.0, static_cast<double>(m_lastDt) * 1.5);

            return m_elapsed + wallDelta;
        }

        void updateVelocity(float dt) {
            auto player = getPlayer();
            if (!player || dt <= 0.f) return;

            float x = player->getPositionX();
            float dx = x - m_lastPlayerX;
            m_lastPlayerX = x;

            float pps = dx / dt;
            if (std::isfinite(pps) && std::abs(pps) > 20.f && std::abs(pps) < 5000.f) {
                m_estimatedPps = pps;
            } else if (std::abs(m_estimatedPps) < 20.f) {
                m_estimatedPps = m_settings.fallbackScrollPps;
            }
        }

        CCPoint playerScreenPosition() const {
            auto player = getPlayer();
            if (!player) return ccp(0.f, 0.f);

            auto parent = player->getParent();
            if (!parent) return player->getPosition();

            auto world = parent->convertToWorldSpace(player->getPosition());
            return const_cast<RhythmGuideLayer*>(this)->convertToNodeSpace(world);
        }

        void updateLines() {
            double lookahead = m_settings.lookaheadMs / 1000.0;
            double past = m_settings.pastVisibleMs / 1000.0;
            double from = m_elapsed - past;
            double to = m_elapsed + lookahead;

            while (m_scanIndex < m_inputs.size() && m_inputs[m_scanIndex].time < from) {
                m_scanIndex++;
            }

            for (auto it = m_activeLines.begin(); it != m_activeLines.end();) {
                auto const& input = m_inputs[it->inputIndex];
                if (input.time < from || input.time > to) {
                    if (it->node) it->node->removeFromParent();
                    it = m_activeLines.erase(it);
                } else {
                    ++it;
                }
            }

            int visible = static_cast<int>(m_activeLines.size());

            for (size_t i = m_scanIndex; i < m_inputs.size(); i++) {
                if (m_inputs[i].time > to) break;
                if (m_inputs[i].time < from) continue;
                if (isLineActive(i)) continue;
                if (visible >= m_settings.maxVisibleLines) break;

                auto node = createLineNode(m_inputs[i]);
                addChild(node, 10);
                m_activeLines.push_back({i, node});
                visible++;
            }

            positionLines();
        }

        bool isLineActive(size_t index) const {
            for (auto const& line : m_activeLines) {
                if (line.inputIndex == index) return true;
            }
            return false;
        }

        CCNode* createLineNode(MacroInput const& input) {
            auto root = CCNode::create();

            auto win = CCDirector::sharedDirector()->getWinSize();
            float height = win.height * std::clamp(m_settings.lineHeightPercent, 0.05f, 1.5f);

            auto lineColor = parseHexColor(m_settings.lineColor, m_settings.lineAlpha);
            auto glowColor = parseHexColor(m_settings.lineColor, m_settings.glowAlpha);
            auto accentColor = parseHexColor(m_settings.accentColor, 0.92f);

            if (m_settings.glowEnabled && m_settings.glowAlpha > 0.f) {
                auto glow = CCDrawNode::create();
                glow->drawSegment(
                    ccp(0.f, -height * 0.5f),
                    ccp(0.f, height * 0.5f),
                    m_settings.lineWidth * 3.2f,
                    glowColor
                );
                root->addChild(glow);
            }

            auto line = CCDrawNode::create();
            line->drawSegment(
                ccp(0.f, -height * 0.5f),
                ccp(0.f, height * 0.5f),
                m_settings.lineWidth,
                lineColor
            );
            root->addChild(line);

            auto dot = CCDrawNode::create();
            dot->drawDot(ccp(0.f, 0.f), m_settings.markerDotSize, accentColor);
            root->addChild(dot, 2);

            if (m_settings.showEventLabels) {
                std::string labelText;
                labelText += input.player == 2 ? "P2 " : "P1 ";
                labelText += input.down ? "IN" : "OUT";

                auto label = CCLabelBMFont::create(labelText.c_str(), "bigFont.fnt");
                label->setScale(0.25f);
                label->setOpacity(150);
                label->setPosition(ccp(0.f, height * 0.5f + 12.f));
                root->addChild(label, 3);
            }

            root->setCascadeOpacityEnabled(true);
            return root;
        }

        void positionLines() {
            auto playerPos = playerScreenPosition();
            auto win = CCDirector::sharedDirector()->getWinSize();

            float pps = std::abs(m_estimatedPps) > 20.f ? std::abs(m_estimatedPps) : m_settings.fallbackScrollPps;

            for (auto const& active : m_activeLines) {
                if (!active.node) continue;

                auto const& input = m_inputs[active.inputIndex];
                double delta = input.time - m_elapsed;

                float x = playerPos.x + static_cast<float>(delta * pps);
                float y = win.height * 0.5f;

                active.node->setPosition(ccp(x, y));

                float fade = 1.f;
                double lookahead = std::max(0.001, m_settings.lookaheadMs / 1000.0);

                if (delta > lookahead * 0.75) {
                    fade = static_cast<float>(1.0 - ((delta - lookahead * 0.75) / (lookahead * 0.25)));
                }

                if (delta < 0.0) {
                    double past = std::max(0.001, m_settings.pastVisibleMs / 1000.0);
                    fade = static_cast<float>(1.0 + delta / past);
                }

                fade = std::clamp(fade, 0.f, 1.f);
                active.node->setOpacity(static_cast<GLubyte>(255.f * fade));
            }
        }

        std::optional<size_t> findBestInput(double now, bool down, int player, int button) {
            double window = m_settings.inputWindowMs / 1000.0;
            double bestAbs = window;
            std::optional<size_t> best;

            for (size_t i = 0; i < m_inputs.size(); i++) {
                auto const& input = m_inputs[i];

                if (input.judged) continue;
                if (input.down != down) continue;

                if (m_settings.strictPlayerMatch && input.player != player) {
                    continue;
                }

                if (m_settings.strictButtonMatch && input.button != button) {
                    continue;
                }

                double diff = now - input.time;
                double absDiff = std::abs(diff);

                if (absDiff <= bestAbs) {
                    bestAbs = absDiff;
                    best = i;
                }

                if (input.time > now + window) {
                    break;
                }
            }

            return best;
        }

        void createFeedbackNode() {
            m_feedbackRoot = CCNode::create();
            m_feedbackRoot->setVisible(false);
            addChild(m_feedbackRoot, 100);

            m_feedbackIcon = CCDrawNode::create();
            m_feedbackIcon->drawDot(ccp(0.f, 0.f), 7.f, ccc4f(1.f, 1.f, 1.f, 0.86f));
            m_feedbackIcon->drawCircle(ccp(0.f, 0.f), 10.f, 0.f, 32, false, ccc4f(1.f, 1.f, 1.f, 0.45f));
            m_feedbackRoot->addChild(m_feedbackIcon);

            m_feedbackLabel = CCLabelBMFont::create("In", "bigFont.fnt");
            m_feedbackLabel->setScale(m_settings.feedbackFontScale);
            m_feedbackLabel->setPosition(ccp(0.f, 22.f));
            m_feedbackRoot->addChild(m_feedbackLabel);
        }

        void showFeedback(double deltaMs) {
            if (isSuppressedByGameMode() && m_settings.hideFeedbackInShipWave) return;

            std::string text;
            double absMs = std::abs(deltaMs);

            if (absMs <= m_settings.inWindowMs && m_settings.showInText) {
                text = "In";
            } else {
                int rounded = static_cast<int>(std::round(deltaMs));
                text = rounded > 0 ? "+" + std::to_string(rounded) + "ms" : std::to_string(rounded) + "ms";
            }

            showRawFeedback(text);
        }

        void showRawFeedback(std::string const& text) {
            if (!m_feedbackRoot || !m_feedbackLabel) return;
            if (isSuppressedByGameMode() && m_settings.hideFeedbackInShipWave) return;

            m_feedbackLabel->setString(text.c_str());
            m_feedbackLabel->setScale(m_settings.feedbackFontScale);
            m_feedbackRoot->setVisible(true);
            m_feedbackRoot->setOpacity(255);

            double duration = m_settings.feedbackDurationMs / 1000.0;
            m_feedbackUntil = m_elapsed + duration;
        }

        void updateFeedbackPosition() {
            if (!m_feedbackRoot) return;

            if (!m_feedbackRoot->isVisible()) return;

            auto pos = playerScreenPosition();
            pos.y += m_settings.feedbackYOffset;
            m_feedbackRoot->setPosition(pos);

            bool keepBecauseHeld = m_settings.attachIconUntilRelease && m_activeHeldInputs > 0;

            if (!keepBecauseHeld && m_elapsed > m_feedbackUntil) {
                m_feedbackRoot->setVisible(false);
                return;
            }

            if (!keepBecauseHeld) {
                double left = std::max(0.0, m_feedbackUntil - m_elapsed);
                double total = std::max(0.001, m_settings.feedbackDurationMs / 1000.0);
                float alpha = static_cast<float>(std::clamp(left / total, 0.0, 1.0));
                m_feedbackRoot->setOpacity(static_cast<GLubyte>(255.f * alpha));
            } else {
                m_feedbackRoot->setOpacity(255);
            }
        }

        void removeAllLines() {
            for (auto const& line : m_activeLines) {
                if (line.node) {
                    line.node->removeFromParent();
                }
            }

            m_activeLines.clear();
        }
    };
}

class $modify(RhythmGuidePlayLayer, PlayLayer) {
    struct Fields {
        RhythmGuideLayer* guide = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        auto guide = RhythmGuideLayer::create(this);
        this->addChild(guide, 9999);
        m_fields->guide = guide;

        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        if (m_fields->guide) {
            auto s = getSettings();
            m_fields->guide->resetGuide(s.reloadMacroOnReset);
        }
    }

    void pushButton(int button, bool player) {
        if (m_fields->guide) {
            m_fields->guide->handleInput(true, player, button);
        }

        PlayLayer::pushButton(button, player);
    }

    void releaseButton(int button, bool player) {
        if (m_fields->guide) {
            m_fields->guide->handleInput(false, player, button);
        }

        PlayLayer::releaseButton(button, player);
    }
};
