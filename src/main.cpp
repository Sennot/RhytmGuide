#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include "Macro.hpp"

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Global macro state
// ---------------------------------------------------------------------------

static RGMacro g_macro;
static std::filesystem::path g_loadedPath;
static std::vector<bool> g_matched;      // press already matched to a real input
static std::vector<uint32_t> g_holdEnds; // for each press: frame of its release (UINT32_MAX = tap)

struct PendingJudgement {
    double expectedTime;
    double actualTime;
};
static std::vector<PendingJudgement> g_pending;

static void rebuildHoldEnds() {
    g_holdEnds.assign(g_macro.inputs.size(), UINT32_MAX);
    for (size_t i = 0; i < g_macro.inputs.size(); i++) {
        auto const& press = g_macro.inputs[i];
        if (!press.down) continue;
        for (size_t j = i + 1; j < g_macro.inputs.size(); j++) {
            auto const& rel = g_macro.inputs[j];
            if (!rel.down && rel.player2 == press.player2 && rel.button == press.button) {
                g_holdEnds[i] = rel.frame;
                break;
            }
        }
    }
}

static void resetAttemptState() {
    g_matched.assign(g_macro.inputs.size(), false);
    g_pending.clear();
}

static void reloadMacro(bool force = false) {
    auto path = Mod::get()->getSettingValue<std::filesystem::path>("macro-file");
    if (!force && path == g_loadedPath) return;
    g_loadedPath = path;
    g_pending.clear();
    if (path.empty() || !std::filesystem::exists(path)) {
        g_macro = RGMacro{};
        g_matched.clear();
        g_holdEnds.clear();
        return;
    }
    g_macro = loadGDRMacro(path);
    if (!g_macro.loaded) {
        log::warn("RhytmGuide: failed to load macro: {}", g_macro.error);
    } else {
        log::info("RhytmGuide: loaded {} inputs @ {} fps (bot: {} {})",
                  g_macro.inputs.size(), g_macro.framerate, g_macro.botName, g_macro.botVersion);
    }
    rebuildHoldEnds();
    resetAttemptState();
}

static double macroFps() {
    double custom = Mod::get()->getSettingValue<double>("custom-fps");
    if (custom > 0.0) return custom;
    return g_macro.framerate > 0.f ? g_macro.framerate : 240.0;
}

static double inputOffsetSec() {
    return Mod::get()->getSettingValue<double>("input-offset-ms") / 1000.0;
}

// ---------------------------------------------------------------------------
// PlayLayer hook: rendering + judgement text lifecycle
// ---------------------------------------------------------------------------

class $modify(RGPlayLayer, PlayLayer) {
    struct Fields {
        CCDrawNode* drawNode = nullptr;
        double lastX = 0.0;
        double lastTime = 0.0;
        float speed = 311.58f; // normal speed px/s, refined every frame
        bool hasSample = false;

        struct FloatingText {
            CCLabelBMFont* label;
            double startTime;
            double fadeTime;
            CCPoint basePos;
        };
        std::vector<FloatingText> texts;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;

        auto f = m_fields.self();
        f->drawNode = CCDrawNode::create();
        f->drawNode->setPosition({0, 0});
        this->m_objectLayer->addChild(f->drawNode, 5000);

        reloadMacro();
        resetAttemptState();
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        resetAttemptState();
        f->hasSample = false;
        f->speed = 311.58f;
        this->clearTexts();
    }

    void clearTexts() {
        auto f = m_fields.self();
        for (auto& t : f->texts) {
            if (t.label) t.label->removeFromParent();
        }
        f->texts.clear();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        auto f = m_fields.self();
        if (!f->drawNode) return;
        f->drawNode->clear();

        auto p1 = this->m_player1;
        if (!p1) return;

        // Game-clock time, perfectly in sync with physics steps.
        double now = this->m_gameState.m_levelTime;

        // Estimate current horizontal speed from real player movement.
        double px = p1->getPositionX();
        if (f->hasSample && now > f->lastTime) {
            double inst = (px - f->lastX) / (now - f->lastTime);
            if (inst > 1.0 && inst < 3000.0) {
                f->speed = f->speed * 0.65f + static_cast<float>(inst) * 0.35f;
            }
        }
        f->lastX = px;
        f->lastTime = now;
        f->hasSample = true;

        bool enabled = Mod::get()->getSettingValue<bool>("enabled");
        bool hideShip = Mod::get()->getSettingValue<bool>("hide-on-ship") && p1->m_isShip;
        bool hideWave = Mod::get()->getSettingValue<bool>("hide-on-wave") && p1->m_isDart;
        bool hidden = !enabled || hideShip || hideWave || !g_macro.loaded;

        if (!hidden) {
            this->renderGuide(now, px);
        }

        // Consume judgements coming from the input hook.
        for (auto const& j : g_pending) {
            if (!hidden && Mod::get()->getSettingValue<bool>("show-timing-text")) {
                this->spawnJudgement(j, now);
            }
        }
        g_pending.clear();

        this->updateTexts(now, hidden);
    }

    void renderGuide(double now, double px) {
        auto f = m_fields.self();
        auto p1 = this->m_player1;

        double approach = Mod::get()->getSettingValue<int64_t>("approach-ms") / 1000.0;
        double offset = inputOffsetSec();
        double fps = macroFps();
        bool showP2 = Mod::get()->getSettingValue<bool>("show-player2");

        auto col = Mod::get()->getSettingValue<cocos2d::ccColor3B>("line-color");
        auto lineOp = static_cast<float>(Mod::get()->getSettingValue<int64_t>("line-opacity")) / 255.f;
        float thick = static_cast<float>(Mod::get()->getSettingValue<double>("line-thickness"));
        ccColor4F lineColor = ccc4f(col.r / 255.f, col.g / 255.f, col.b / 255.f, lineOp);

        float py = p1->getPositionY();

        auto markerOp = static_cast<float>(Mod::get()->getSettingValue<int64_t>("marker-opacity")) / 255.f;
        float markerR = static_cast<float>(Mod::get()->getSettingValue<double>("marker-size"));
        double tapDur = Mod::get()->getSettingValue<int64_t>("tap-marker-ms") / 1000.0;
        ccColor4F markerColor = ccc4f(1.f, 1.f, 1.f, markerOp);
        CCPoint playerPos = p1->getPosition();

        for (size_t i = 0; i < g_macro.inputs.size(); i++) {
            auto const& in = g_macro.inputs[i];
            if (!in.down) continue;
            if (in.player2 && !showP2) continue;

            double t = in.frame / fps + offset;
            double until = t - now;

            // White marker attached to the player while the input is active.
            if (now >= t) {
                double end = (g_holdEnds[i] != UINT32_MAX)
                                 ? g_holdEnds[i] / fps + offset
                                 : t + tapDur;
                if (now <= end) {
                    f->drawNode->drawDot(playerPos, markerR, markerColor);
                }
                if (now > end) continue; // long past, skip line
            }

            // Guide line approaching the player.
            if (until > approach) break; // inputs are sorted by frame
            if (until < -0.05) continue;

            float x = static_cast<float>(px + until * f->speed);
            f->drawNode->drawSegment({x, py - 340.f}, {x, py + 340.f}, thick, lineColor);
        }
    }

    void spawnJudgement(PendingJudgement const& j, double now) {
        auto f = m_fields.self();
        double devMs = (j.actualTime - j.expectedTime) * 1000.0;
        double perfect = Mod::get()->getSettingValue<double>("perfect-ms");

        std::string text;
        ccColor3B color;
        double absDev = std::fabs(devMs);
        if (absDev <= perfect) {
            text = "In";
            color = {170, 255, 195};
        } else if (devMs > 0) {
            text = fmt::format("+{:.0f} ms", devMs);
            color = {255, 200, 130};
        } else {
            text = fmt::format("-{:.0f} ms", -devMs);
            color = {150, 205, 255};
        }

        auto label = CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
        label->setScale(static_cast<float>(Mod::get()->getSettingValue<double>("text-scale")));
        label->setColor(color);

        CCPoint base = this->m_player1->getPosition() + CCPoint{0.f, 38.f};
        label->setPosition(base);
        label->setAnchorPoint({0.5f, 0.f});
        this->m_objectLayer->addChild(label, 5001);

        f->texts.push_back({label, now,
                            Mod::get()->getSettingValue<int64_t>("text-fade-ms") / 1000.0, base});
    }

    void updateTexts(double now, bool hidden) {
        auto f = m_fields.self();
        auto& texts = f->texts;
        for (size_t i = 0; i < texts.size();) {
            auto& t = texts[i];
            if (!t.label || hidden) {
                if (t.label) t.label->removeFromParent();
                texts.erase(texts.begin() + i);
                continue;
            }
            double age = now - t.startTime;
            if (age >= t.fadeTime) {
                t.label->removeFromParent();
                texts.erase(texts.begin() + i);
                continue;
            }
            float k = t.fadeTime > 0.0 ? static_cast<float>(age / t.fadeTime) : 1.f;
            t.label->setOpacity(static_cast<GLubyte>(255.f * (1.f - k)));
            t.label->setPosition(t.basePos + CCPoint{0.f, 42.f * k});
            i++;
        }
    }
};

// ---------------------------------------------------------------------------
// Input observation hook (CBF-safe: we never alter input handling, we only
// read the game clock after the game has processed the button).
// ---------------------------------------------------------------------------

class $modify(RGInputHook, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (!down) return;
        if (!g_macro.loaded) return;
        if (!Mod::get()->getSettingValue<bool>("enabled")) return;

        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) return;

        auto p1 = pl->m_player1;
        if (!p1) return;
        if (Mod::get()->getSettingValue<bool>("hide-on-ship") && p1->m_isShip) return;
        if (Mod::get()->getSettingValue<bool>("hide-on-wave") && p1->m_isDart) return;

        bool p2 = !isPlayer1;
        if (p2 && !Mod::get()->getSettingValue<bool>("show-player2")) return;

        double now = pl->m_gameState.m_levelTime;
        double fps = macroFps();
        double offset = inputOffsetSec();
        double window = Mod::get()->getSettingValue<int64_t>("match-window-ms") / 1000.0;

        size_t best = SIZE_MAX;
        double bestAbs = window;
        for (size_t i = 0; i < g_macro.inputs.size(); i++) {
            auto const& in = g_macro.inputs[i];
            if (!in.down || in.player2 != p2) continue;
            if (i < g_matched.size() && g_matched[i]) continue;
            double t = in.frame / fps + offset;
            double d = std::fabs(now - t);
            if (d <= bestAbs) {
                bestAbs = d;
                best = i;
            }
        }
        if (best == SIZE_MAX) return;

        if (best < g_matched.size()) g_matched[best] = true;
        g_pending.push_back({g_macro.inputs[best].frame / fps + offset, now});
    }
};

// ---------------------------------------------------------------------------

$on_mod(Loaded) {
    reloadMacro(true);
    listenForSettingChanges("macro-file", [](std::filesystem::path const&) {
        reloadMacro(true);
    });
}
