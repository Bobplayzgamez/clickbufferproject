#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;

// Click Buffer's input timing implementation is adapted from the MIT-licensed
// Input Buffer project by Lyophi. See LICENSE for the required notice.

// ---- settings --------------------------------------------------------
static constexpr char const* STAGE_NAMES[] = {"2 ticks", "4 ticks", "8 ticks", "16 ticks", "Off"};
static constexpr int STAGES[] = {2, 4, 8, 16, 0};
static constexpr int STAGE_COUNT = 5;
static int g_stage = 1;

static constexpr int WAVE_MAX_TICKS = 8;
static constexpr int SPAWN_GRACE_TICKS = 16;
// Index: 0 cube, 1 ball, 2 robot, 3 spider, 4 ship, 5 ufo, 6 swing, 7 wave.
// Value: -1 = same as main window, 0 = off, >0 = custom ticks.
static constexpr int MODE_COUNT = 8;
static constexpr int MODE_WAVE = 7;
static constexpr char const* MODE_KEYS[] = {
    "cube-ticks", "ball-ticks", "robot-ticks", "spider-ticks",
    "ship-ticks", "ufo-ticks", "swing-ticks", "wave-ticks"
};
static int g_modeTicks[MODE_COUNT] = {-1, -1, -1, -1, -1, -1, -1, -1};
static constexpr char const* WAVE_NAMES[] = {"Min Hold", "Min Gap", "Grid"};
enum WaveMode { WAVE_HOLD = 0, WAVE_GAP = 1, WAVE_GRID = 2 };
static constexpr int WAVE_MODE_COUNT = 3;
static int g_waveMode = WAVE_GRID;

static constexpr char const* TPS_NAMES[] = {"60", "120", "240", "360", "480", "720", "960", "1000"};
static constexpr int TPS_PRESETS[] = {60, 120, 240, 360, 480, 720, 960, 1000};
static constexpr int TPS_COUNT = 8;
static int g_tpsIndex = 2;

static bool g_debug = false;
// Hotkeys: 0 main window, 1 TPS up, 2 TPS down, 3 debug, 4 wave mode.
static constexpr int KEY_COUNT = 5;
static constexpr char const* KEY_SETTINGS[] = {"key-stage", "key-tps-up", "key-tps-down", "key-debug", "key-wave"};
static int g_keyVK[KEY_COUNT] = {0, 0, 0, 0, 0}; // 0 = none

// Raised whenever the level resets (death, restart, checkpoint).
static int g_resetEpoch = 0;

// Predictive cube jump timing. This uses checkpoint simulation and is experimental
// until compiled and tested against the target Geometry Dash build.
static bool g_safeJumpEnabled = true;
static int g_safeJumpFrames = 5;
static int g_safeJumpHorizon = 90;
static bool g_safeSimulation = false;
static bool g_safeSimDied = false;
static bool g_safeSimFired = false;
static bool g_safeSimCompleted = false;
static int g_safeSimPlayer = -1;
static int g_safeSimCandidateDelay = 0;
static int g_safeSimElapsed = 0;
static int g_safeSimReleaseAt = -1;
static bool g_safePending[2] = {false, false};
static bool g_safePlanReady[2] = {false, false};
static int g_safeDelay[2] = {0, 0};
static int g_safeAge[2] = {0, 0};
static bool g_safeNoCandidateNotified[2] = {false, false};
static bool g_safeTapJustSent[2] = {false, false};
// ----------------------------------------------------------------------
#define DBG(...) do { if (g_debug) log::info(__VA_ARGS__); } while (0)

static int indexOf(char const* const* names, int n, std::string const& v, int def) {
    for (int i = 0; i < n; i++) if (v == names[i]) return i;
    return def;
}

static int vkFromName(std::string const& n) {
#ifdef GEODE_IS_WINDOWS
    if (n == "None") return 0;
    if (n.size() >= 2 && n[0] == 'F') {
        int num = std::atoi(n.c_str() + 1);
        if (num >= 1 && num <= 12) return VK_F1 + (num - 1);
    }
    if (n == "Insert") return VK_INSERT;
    if (n == "Delete") return VK_DELETE;
    if (n == "Home") return VK_HOME;
    if (n == "End") return VK_END;
    if (n == "Page Up") return VK_PRIOR;
    if (n == "Page Down") return VK_NEXT;
#endif
    return 0;
}

static void loadSettings() {
    auto m = Mod::get();
    g_stage = indexOf(STAGE_NAMES, STAGE_COUNT, m->getSettingValue<std::string>("stage"), 1);
    g_tpsIndex = indexOf(TPS_NAMES, TPS_COUNT, m->getSettingValue<std::string>("tps"), 2);
    g_waveMode = indexOf(WAVE_NAMES, WAVE_MODE_COUNT, m->getSettingValue<std::string>("wave-mode"), WAVE_GRID);
    for (int i = 0; i < MODE_COUNT; i++) {
        g_modeTicks[i] = static_cast<int>(m->getSettingValue<int64_t>(MODE_KEYS[i]));
    }
    for (int i = 0; i < KEY_COUNT; i++) {
        g_keyVK[i] = vkFromName(m->getSettingValue<std::string>(KEY_SETTINGS[i]));
    }
    g_debug = m->getSettingValue<bool>("debug");
    g_safeJumpEnabled = m->getSettingValue<bool>("safe-jump");
    g_safeJumpFrames = static_cast<int>(std::clamp<int64_t>(m->getSettingValue<int64_t>("safe-jump-frames"), 1, 10));
    g_safeJumpHorizon = static_cast<int>(std::clamp<int64_t>(m->getSettingValue<int64_t>("safe-jump-horizon"), 30, 200));
}

static char const* waveModeName() { return WAVE_NAMES[g_waveMode]; }

static float stepsPerTick() {
    // The physics simulation is 240 steps/sec. One timing tick at selected TPS
    // therefore spans 240 / TPS physics steps (e.g. 4 steps at 60 TPS).
    return 240.0f / TPS_PRESETS[g_tpsIndex];
}

static int mainLimitSteps() {
    return static_cast<int>(std::lround(STAGES[g_stage] * stepsPerTick()));
}

static bool isFlyMode(PlayerObject* p) {
    return p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing;
}

static bool isGroundMode(PlayerObject* p) {
    return !isFlyMode(p);
}

// Predictive safe-timing currently targets cube only. Ball, robot and spider
// have mode-specific input behavior that should not be silently treated as cube.
static bool isCubeMode(PlayerObject* p) {
    return !isFlyMode(p) && !p->m_isBall && !p->m_isRobot && !p->m_isSpider;
}

static int safeJumpWindowSteps() {
    // At 240 TPS one configured frame is one physics step; at 60 TPS it is four.
    return std::max(1, static_cast<int>(std::lround(g_safeJumpFrames * stepsPerTick())));
}

static int modeIndex(PlayerObject* p) {
    if (p->m_isShip) return 4;
    if (p->m_isBird) return 5;
    if (p->m_isDart) return 7;
    if (p->m_isSwing) return 6;
    if (p->m_isBall) return 1;
    if (p->m_isRobot) return 2;
    if (p->m_isSpider) return 3;
    return 0;
}

static int ticksForMode(int idx) {
    int c = g_modeTicks[idx];
    if (c < 0) {
        int t = STAGES[g_stage];
        if (idx == MODE_WAVE && t > 0) t = std::min(t, WAVE_MAX_TICKS);
        return t;
    }
    return c;
}

static int modeLimitSteps(PlayerObject* p) {
    int ticks = ticksForMode(modeIndex(p));
    if (ticks <= 0) return 0;
    return std::max(1, static_cast<int>(std::lround(ticks * stepsPerTick())));
}

static int waveGridSteps() {
    int ticks = ticksForMode(MODE_WAVE);
    if (ticks <= 0) return 0;
    return std::max(1, static_cast<int>(std::lround(ticks * stepsPerTick())));
}

static int spawnGraceSteps() {
    return std::max(1, static_cast<int>(std::lround(SPAWN_GRACE_TICKS * stepsPerTick())));
}

static int ringsTouching(PlayerObject* p) {
    return p->m_touchingRings ? static_cast<int>(p->m_touchingRings->count()) : 0;
}

static char const* modeName(PlayerObject* p) {
    if (p->m_isShip) return "ship";
    if (p->m_isBird) return "ufo";
    if (p->m_isDart) return "wave";
    if (p->m_isSwing) return "swing";
    if (p->m_isBall) return "ball";
    if (p->m_isRobot) return "robot";
    if (p->m_isSpider) return "spider";
    return "cube";
}

static void doHotkey(int action) {
    auto m = Mod::get();
    switch (action) {
        case 0:
            g_stage = (g_stage + 1) % STAGE_COUNT;
            m->setSettingValue<std::string>("stage", STAGE_NAMES[g_stage]);
            log::info("[ClickBuffer] Main window -> {} ({} steps, wave {} steps @ {} TPS)",
                      STAGE_NAMES[g_stage], mainLimitSteps(), waveGridSteps(), TPS_PRESETS[g_tpsIndex]);
            break;
        case 1:
            g_tpsIndex = std::min(g_tpsIndex + 1, TPS_COUNT - 1);
            m->setSettingValue<std::string>("tps", TPS_NAMES[g_tpsIndex]);
            log::info("[ClickBuffer] TPS -> {} ({:.2f} steps/tick)", TPS_PRESETS[g_tpsIndex], stepsPerTick());
            break;
        case 2:
            g_tpsIndex = std::max(g_tpsIndex - 1, 0);
            m->setSettingValue<std::string>("tps", TPS_NAMES[g_tpsIndex]);
            log::info("[ClickBuffer] TPS -> {} ({:.2f} steps/tick)", TPS_PRESETS[g_tpsIndex], stepsPerTick());
            break;
        case 3:
            g_debug = !g_debug;
            m->setSettingValue<bool>("debug", g_debug);
            log::info("[ClickBuffer] Debug logging -> {}", g_debug ? "ON" : "OFF");
            break;
        case 4:
            g_waveMode = (g_waveMode + 1) % WAVE_MODE_COUNT;
            m->setSettingValue<std::string>("wave-mode", WAVE_NAMES[g_waveMode]);
            log::info("[ClickBuffer] Wave mode -> {} ({} steps)", waveModeName(), waveGridSteps());
            break;
    }
}

static void pollKeys() {
#ifdef GEODE_IS_WINDOWS
    static bool prev[KEY_COUNT] = {false, false, false, false, false};
    bool now[KEY_COUNT];
    for (int i = 0; i < KEY_COUNT; i++) {
        now[i] = g_keyVK[i] != 0 && (GetAsyncKeyState(g_keyVK[i]) & 0x8000) != 0;
    }
    for (int i = 0; i < KEY_COUNT; i++) {
        if (now[i] && !prev[i]) doHotkey(i);
    }
    for (int i = 0; i < KEY_COUNT; i++) prev[i] = now[i];
#endif
}

$on_mod(Loaded) {
    loadSettings();
    listenForSettingChanges<std::string>("stage", [](std::string) { loadSettings(); });
    listenForSettingChanges<std::string>("tps", [](std::string) { loadSettings(); });
    listenForSettingChanges<std::string>("wave-mode", [](std::string) { loadSettings(); });
    for (auto key : MODE_KEYS) {
        listenForSettingChanges<int64_t>(key, [](int64_t) { loadSettings(); });
    }
    for (auto key : KEY_SETTINGS) {
        listenForSettingChanges<std::string>(key, [](std::string) { loadSettings(); });
    }
    listenForSettingChanges<bool>("debug", [](bool) { loadSettings(); });
    listenForSettingChanges<bool>("safe-jump", [](bool) { loadSettings(); });
    listenForSettingChanges<int64_t>("safe-jump-frames", [](int64_t) { loadSettings(); });
    listenForSettingChanges<int64_t>("safe-jump-horizon", [](int64_t) { loadSettings(); });
    log::info("[ClickBuffer] loaded. Main: {}, TPS: {}, wave: {}",
              STAGE_NAMES[g_stage], TPS_PRESETS[g_tpsIndex], waveModeName());
}

// A fully code-drawn popup for the Safe Jump Timing controls. The slider tracks
// use discrete clickable ticks, which avoids adding image resources to the mod.
class SafeJumpSettingsPopup : public Popup {
protected:
    CCMenu* m_controls = nullptr;
    CCMenuItemToggler* m_enabledToggle = nullptr;
    CCLabelBMFont* m_enabledLabel = nullptr;
    CCLabelBMFont* m_windowValue = nullptr;
    CCLabelBMFont* m_horizonValue = nullptr;
    std::vector<CCLayerColor*> m_windowMarks;
    std::vector<CCLayerColor*> m_horizonMarks;

    static ccColor3B cyan() { return {0, 226, 255}; }
    static ccColor3B dimBlue() { return {26, 83, 112}; }
    static ccColor3B paleText() { return {224, 244, 255}; }
    static ccColor3B mutedText() { return {115, 160, 192}; }
    static ccColor3B warningColor() { return {255, 190, 67}; }

    CCLabelBMFont* addLabel(char const* text, float scale, CCPoint pos, ccColor3B color, bool leftAligned = false) {
        auto label = CCLabelBMFont::create(text, "bigFont.fnt");
        if (!label) return nullptr;
        label->setScale(scale);
        label->setColor(color);
        label->setAnchorPoint(leftAligned ? CCPoint{0.f, 0.5f} : CCPoint{0.5f, 0.5f});
        label->setPosition(pos);
        m_mainLayer->addChild(label);
        return label;
    }

    CCLayerColor* addRect(ccColor4B color, float width, float height, CCPoint pos) {
        auto rect = CCLayerColor::create(color, width, height);
        if (!rect) return nullptr;
        rect->setAnchorPoint({0.5f, 0.5f});
        rect->setPosition(pos);
        m_mainLayer->addChild(rect);
        return rect;
    }

    void makeTickSlider(int minValue, int maxValue, int step, float y, bool windowSlider) {
        constexpr float trackWidth = 270.f;
        float cx = m_mainLayer->getContentSize().width / 2.f;
        float spacing = trackWidth / static_cast<float>((maxValue - minValue) / step);

        // Subtle track; the tick buttons above it are the actual interactive controls.
        addRect({14, 48, 69, 255}, trackWidth + 4.f, 5.f, {cx, y});
        auto ticks = CCMenu::create();
        ticks->setPosition({cx, y});
        m_controls->addChild(ticks);

        for (int value = minValue; value <= maxValue; value += step) {
            auto mark = CCLayerColor::create({26, 83, 112, 255}, 8.f, 17.f);
            if (!mark) continue;
            mark->setAnchorPoint({0.5f, 0.5f});
            auto item = CCMenuItemSpriteExtra::create(
                mark,
                this,
                windowSlider
                    ? menu_selector(SafeJumpSettingsPopup::onWindowTick)
                    : menu_selector(SafeJumpSettingsPopup::onHorizonTick)
            );
            item->setTag(value);
            item->setPosition({-trackWidth / 2.f + spacing * ((value - minValue) / step), 0.f});
            ticks->addChild(item);
            if (windowSlider) m_windowMarks.push_back(mark);
            else m_horizonMarks.push_back(mark);
        }

        addLabel(fmt::format("{}", minValue).c_str(), 0.32f, {cx - trackWidth / 2.f, y - 17.f}, mutedText());
        addLabel(fmt::format("{}", maxValue).c_str(), 0.32f, {cx + trackWidth / 2.f, y - 17.f}, mutedText());
    }

    void refreshValues() {
        auto mod = Mod::get();
        bool enabled = mod->getSettingValue<bool>("safe-jump");
        int frames = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("safe-jump-frames"), 1, 10));
        int horizon = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("safe-jump-horizon"), 30, 200));

        if (m_enabledToggle && m_enabledToggle->isToggled() != enabled) {
            m_enabledToggle->toggle(enabled);
        }
        if (m_enabledLabel) {
            m_enabledLabel->setString(enabled ? "ON" : "OFF");
            m_enabledLabel->setColor(enabled ? cyan() : mutedText());
        }
        if (m_windowValue) m_windowValue->setString(fmt::format("{}", frames).c_str());
        if (m_horizonValue) m_horizonValue->setString(fmt::format("{}", horizon).c_str());

        for (int value = 1; value <= 10; ++value) {
            size_t index = static_cast<size_t>(value - 1);
            if (index < m_windowMarks.size()) {
                m_windowMarks[index]->setColor(value == frames ? cyan() : dimBlue());
                m_windowMarks[index]->setOpacity(value == frames ? 255 : 200);
            }
        }
        size_t index = 0;
        for (int value = 30; value <= 200; value += 10, ++index) {
            if (index < m_horizonMarks.size()) {
                // The horizon slider moves in 10-step increments (30, 40, ... 200).
                m_horizonMarks[index]->setColor(value == horizon ? cyan() : dimBlue());
                m_horizonMarks[index]->setOpacity(value == horizon ? 255 : 200);
            }
        }
    }

    bool init() {
        if (!Popup::init(460.f, 360.f)) return false;
        this->setTitle("Click Buffer");
        auto size = m_mainLayer->getContentSize();
        float cx = size.width / 2.f;
        float h = size.height;

        // Dark panel and cyan frame, drawn entirely with Cocos nodes (no image assets).
        addRect({3, 14, 27, 245}, size.width - 14.f, size.height - 29.f, {cx, (h - 12.f) / 2.f});
        addRect({0, 194, 229, 255}, size.width - 18.f, 2.f, {cx, h - 29.f});
        addRect({0, 194, 229, 255}, size.width - 18.f, 2.f, {cx, 18.f});
        addRect({0, 194, 229, 255}, 2.f, h - 47.f, {9.f, (h - 29.f) / 2.f});
        addRect({0, 194, 229, 255}, 2.f, h - 47.f, {size.width - 9.f, (h - 29.f) / 2.f});
        addRect({0, 220, 255, 255}, size.width - 28.f, 3.f, {cx, h - 30.f});

        addLabel("SAFE JUMP TIMING", 0.48f, {cx - 188.f, h - 59.f}, paleText(), true);
        addLabel("Test the current jump and short delays before sending the click.",
                 0.30f, {cx - 188.f, h - 80.f}, mutedText(), true);

        m_controls = CCMenu::create();
        m_controls->setPosition({0.f, 0.f});
        m_mainLayer->addChild(m_controls);
        m_enabledToggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(SafeJumpSettingsPopup::onSafeJumpToggle), 0.72f
        );
        m_enabledToggle->setPosition({cx + 164.f, h - 66.f});
        m_enabledToggle->toggle(Mod::get()->getSettingValue<bool>("safe-jump"));
        m_controls->addChild(m_enabledToggle);
        m_enabledLabel = addLabel(Mod::get()->getSettingValue<bool>("safe-jump") ? "ON" : "OFF",
                                  0.34f, {cx + 190.f, h - 66.f}, cyan());

        addRect({17, 62, 88, 255}, size.width - 50.f, 1.5f, {cx, h - 103.f});
        addLabel("SAFE JUMP WINDOW", 0.40f, {cx - 188.f, h - 123.f}, paleText(), true);
        addLabel("How many frames ahead to test (1-10).", 0.30f,
                 {cx - 188.f, h - 143.f}, mutedText(), true);
        m_windowValue = addLabel("5", 0.44f, {cx + 179.f, h - 166.f}, paleText());
        makeTickSlider(1, 10, 1, h - 166.f, true);

        addLabel("PREDICTION HORIZON", 0.40f, {cx - 188.f, h - 208.f}, paleText(), true);
        addLabel("Physics steps simulated after each test jump.", 0.30f,
                 {cx - 188.f, h - 228.f}, mutedText(), true);
        m_horizonValue = addLabel("90", 0.40f, {cx + 179.f, h - 251.f}, paleText());
        makeTickSlider(30, 200, 10, h - 251.f, false);

        // A compact amber warning at the bottom of the popup.
        addRect({65, 48, 15, 255}, 280.f, 43.f, {cx - 68.f, 38.f});
        addRect({255, 180, 45, 255}, 280.f, 2.f, {cx - 68.f, 59.f});
        addRect({255, 180, 45, 255}, 280.f, 2.f, {cx - 68.f, 17.f});
        addLabel("EXPERIMENTAL FEATURE", 0.31f, {cx - 198.f, 48.f}, warningColor(), true);
        addLabel("May stutter; predictions can differ from live play.",
                 0.25f, {cx - 198.f, 31.f}, paleText(), true);

        auto resetButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Reset"), this, menu_selector(SafeJumpSettingsPopup::onResetDefaults)
        );
        resetButton->setScale(0.62f);
        resetButton->setPosition({cx + 160.f, 38.f});
        m_controls->addChild(resetButton);

        this->refreshValues();
        return true;
    }

    void onSafeJumpToggle(CCObject*) {
        bool next = !m_enabledToggle->isToggled();
        Mod::get()->setSettingValue<bool>("safe-jump", next);
        g_safeJumpEnabled = next;
        if (m_enabledLabel) {
            m_enabledLabel->setString(next ? "ON" : "OFF");
            m_enabledLabel->setColor(next ? cyan() : mutedText());
        }
    }

    void onWindowTick(CCObject* sender) {
        int value = static_cast<CCNode*>(sender)->getTag();
        value = std::clamp(value, 1, 10);
        Mod::get()->setSettingValue<int64_t>("safe-jump-frames", value);
        loadSettings();
        this->refreshValues();
    }

    void onHorizonTick(CCObject* sender) {
        int value = std::clamp(static_cast<CCNode*>(sender)->getTag(), 30, 200);
        Mod::get()->setSettingValue<int64_t>("safe-jump-horizon", value);
        loadSettings();
        this->refreshValues();
    }

    void onResetDefaults(CCObject*) {
        Mod::get()->setSettingValue<bool>("safe-jump", true);
        Mod::get()->setSettingValue<int64_t>("safe-jump-frames", 5);
        Mod::get()->setSettingValue<int64_t>("safe-jump-horizon", 90);
        loadSettings();
        this->refreshValues();
    }

public:
    static SafeJumpSettingsPopup* create() {
        auto ret = new SafeJumpSettingsPopup();
        if (ret->init()) {
            ret->setID("click-buffer-settings"_spr);
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(BufferPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto menu = CCMenu::create();
        auto spr = ButtonSprite::create("Click Buffer");
        spr->setScale(0.58f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BufferPause::onBufferSettings));
        menu->addChild(btn);

        auto win = CCDirector::get()->getWinSize();
        menu->setPosition({win.width - 72.f, 30.f});
        this->addChild(menu);
    }

    void onBufferSettings(CCObject*) {
        if (auto popup = SafeJumpSettingsPopup::create()) {
            popup->show();
        }
    }
};

// Clear cached input after death, restart, or checkpoint reload.
class $modify(BufferReset, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        ++g_resetEpoch;
        g_safeSimulation = false;
        g_safeSimDied = false;
        g_safeSimFired = false;
        g_safeSimCompleted = false;
        g_safeSimPlayer = -1;
        g_safeSimElapsed = 0;
        g_safeSimReleaseAt = -1;
        for (int i = 0; i < 2; ++i) {
            g_safePending[i] = false;
            g_safePlanReady[i] = false;
            g_safeDelay[i] = 0;
            g_safeAge[i] = 0;
            g_safeNoCandidateNotified[i] = false;
            g_safeTapJustSent[i] = false;
        }
        return true;
    }

    void simulateOnePhysicsStep() {
        int before = g_safeSimElapsed;
        for (int i = 0; i < 8 && g_safeSimElapsed == before && !g_safeSimDied && !g_safeSimCompleted; ++i) {
            PlayLayer::update(1.f / 240.f);
            if ((m_player1 && m_player1->m_isDead) || (m_player2 && m_player2->m_isDead)) {
                g_safeSimDied = true;
            }
        }
    }

    void planSafeJump(int idx) {
        if (g_safeSimulation || idx < 0 || idx > 1 || !g_safePending[idx] || g_safePlanReady[idx]) return;
        auto player = idx == 0 ? m_player1 : m_player2;
        if (!player || player->m_isDead || !player->m_isOnGround) {
            // A queued ground jump is only valid while the cube remains alive and grounded.
            // Never fire a stale click after the player has walked/fallen off an edge.
            g_safePending[idx] = false;
            g_safePlanReady[idx] = false;
            g_safeDelay[idx] = 0;
            g_safeAge[idx] = 0;
            DBG("[ClickBuffer] safe jump cancelled: player unavailable, dead, or no longer grounded");
            return;
        }

        auto checkpoint = this->createCheckpoint();
        if (!checkpoint) {
            // Do not fire a jump we could not evaluate: the user explicitly asked
            // for timing that waits rather than blindly pressing into a hazard.
            g_safePending[idx] = true;
            g_safePlanReady[idx] = false;
            if (!g_safeNoCandidateNotified[idx]) {
                log::warn("[ClickBuffer] Could not create a prediction checkpoint; safe timing is waiting. Try a practice level if this persists.");
                g_safeNoCandidateNotified[idx] = true;
            }
            return;
        }
        checkpoint->retain();

        int maxDelay = safeJumpWindowSteps();
        int horizon = g_safeJumpHorizon;
        int bestDelay = -1;
        int savedEpoch = g_resetEpoch;

        g_safeSimulation = true;
        g_safeSimPlayer = idx;
        for (int candidate = 0; candidate <= maxDelay; ++candidate) {
            if (m_player1) m_player1->releaseButton(PlayerButton::Jump);
            if (m_player2) m_player2->releaseButton(PlayerButton::Jump);
            PlayLayer::loadFromCheckpoint(checkpoint);
            g_resetEpoch = savedEpoch;

            g_safeSimDied = false;
            g_safeSimFired = false;
            g_safeSimCompleted = false;
            g_safeSimElapsed = 0;
            g_safeSimReleaseAt = -1;
            g_safeSimCandidateDelay = candidate;

            // Include the delay and then the configured horizon after the click.
            int trialSteps = horizon + candidate + 1;
            for (int step = 0; step < trialSteps && !g_safeSimDied && !g_safeSimCompleted; ++step) {
                int before = g_safeSimElapsed;
                simulateOnePhysicsStep();
                if (g_safeSimElapsed == before) break;
            }

            if (g_safeSimFired && !g_safeSimDied) {
                bestDelay = candidate;
                break; // first candidate that survives is the preferred timing
            }
        }

        if (m_player1) m_player1->releaseButton(PlayerButton::Jump);
        if (m_player2) m_player2->releaseButton(PlayerButton::Jump);
        PlayLayer::loadFromCheckpoint(checkpoint);
        g_resetEpoch = savedEpoch;
        checkpoint->release();
        g_safeSimulation = false;
        g_safeSimDied = false;
        g_safeSimFired = false;
        g_safeSimCompleted = false;
        g_safeSimPlayer = -1;
        g_safeSimElapsed = 0;
        g_safeSimReleaseAt = -1;

        // If no candidate survives, leave the input queued and retry from the next
        // real frame. Do not force a known-unsafe immediate jump; the queue is cleared
        // on death/restart or if the player is no longer grounded.
        g_safePending[idx] = true;
        if (bestDelay >= 0) {
            g_safePlanReady[idx] = true;
            g_safeDelay[idx] = bestDelay;
            g_safeAge[idx] = 0;
            g_safeNoCandidateNotified[idx] = false;
            log::info("[ClickBuffer] Safe jump selected delay {} / {} physics steps ({} prediction steps).",
                      bestDelay, maxDelay, horizon);
        } else {
            g_safePlanReady[idx] = false;
            g_safeDelay[idx] = 0;
            if (!g_safeNoCandidateNotified[idx]) {
                log::warn("[ClickBuffer] No survivable timing found in the current {}-step lookahead; retrying next frame.", maxDelay);
                g_safeNoCandidateNotified[idx] = true;
            }
        }
    }

    void update(float dt) {
        // Run prediction before the real frame advances. Nested update calls made
        // by simulateOnePhysicsStep are bypassed by g_safeSimulation.
        if (!g_safeSimulation) {
            if (!g_safeJumpEnabled) {
                for (int i = 0; i < 2; ++i) {
                    if (g_safePending[i]) {
                        g_safePlanReady[i] = true;
                        g_safeDelay[i] = 0;
                        g_safeNoCandidateNotified[i] = false;
                    }
                }
            } else {
                if (g_safePending[0] && !g_safePlanReady[0]) planSafeJump(0);
                if (g_safePending[1] && !g_safePlanReady[1]) planSafeJump(1);
            }
        }
        PlayLayer::update(dt);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (!g_safeSimulation) g_resetEpoch++;
    }

    void resetLevelFromStart() {
        PlayLayer::resetLevelFromStart();
        if (!g_safeSimulation) g_resetEpoch++;
    }

    void loadFromCheckpoint(CheckpointObject* object) {
        PlayLayer::loadFromCheckpoint(object);
        if (!g_safeSimulation) g_resetEpoch++;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_safeSimulation) {
            if (object != m_anticheatSpike) g_safeSimDied = true;
            return; // record the collision, then restore the trial checkpoint
        }
        PlayLayer::destroyPlayer(player, object);
        g_resetEpoch++;
    }

    void levelComplete() {
        if (g_safeSimulation) {
            g_safeSimCompleted = true;
            return;
        }
        PlayLayer::levelComplete();
    }
};

struct PState {
    bool held = false;
    bool pending = false;
    int pendingStart = 0;
    int lastRingCount = 0;
    bool releaseNext = false;

    int pressStep = -1;
    bool delayedRelease = false;
    int releaseAt = 0;

    std::vector<std::pair<bool, int>> waveQueue;
    int lastReleaseStep = -1000000;

};

class $modify(BufferLayer, GJBaseGameLayer) {
    struct Fields {
        int step = 0;
        int epoch = 0;
        int graceUntil = 0;
        PState ps[2];
    };

    // In a one-player level with a dual portal, the game's single input affects
    // both icons. Rescued landing clicks are only sent to the second icon if it
    // is also in a ground/ring mode.
    bool sharedDualInput() {
        return m_gameState.m_isDualMode && m_levelSettings && !m_levelSettings->m_twoPlayerMode;
    }

    void sendInput(PlayerObject* p, int idx, bool down, bool groundOnly) {
        auto one = [&](PlayerObject* q) {
            if (down) q->pushButton(PlayerButton::Jump);
            else q->releaseButton(PlayerButton::Jump);
        };
        one(p);
        if (idx == 0 && m_player2 && sharedDualInput()) {
            if (!groundOnly || isGroundMode(m_player2)) one(m_player2);
        }
    }

    void syncState() {
        auto f = m_fields.operator->();
        if (f->epoch == g_resetEpoch) return;
        f->epoch = g_resetEpoch;

        bool inconsistent = false;
        for (auto& s : f->ps) {
            if (s.delayedRelease || s.releaseNext || !s.waveQueue.empty()) inconsistent = true;
            s = PState();
        }
        for (int i = 0; i < 2; ++i) {
            g_safePending[i] = false;
            g_safePlanReady[i] = false;
            g_safeDelay[i] = 0;
            g_safeAge[i] = 0;
            g_safeNoCandidateNotified[i] = false;
            g_safeTapJustSent[i] = false;
        }
        f->graceUntil = f->step + spawnGraceSteps();
        if (inconsistent) {
            if (m_player1) m_player1->releaseButton(PlayerButton::Jump);
            if (m_player2) m_player2->releaseButton(PlayerButton::Jump);
        }
        DBG("[ClickBuffer] state reset; spawn grace until step {}{}",
            f->graceUntil, inconsistent ? ", released stuck button" : "");
    }

    void flushWaveQueue(PlayerObject* p, PState& s, int idx) {
        for (auto const& ev : s.waveQueue) sendInput(p, idx, ev.first, false);
        s.waveQueue.clear();
    }

    void fireSafeTap(PlayerObject* p, int idx) {
        g_safePending[idx] = false;
        g_safePlanReady[idx] = false;
        g_safeDelay[idx] = 0;
        g_safeAge[idx] = 0;
        g_safeNoCandidateNotified[idx] = false;
        sendInput(p, idx, true, true);
        // Schedule release for the next physics step, not the current one.
        g_safeTapJustSent[idx] = true;
        if (idx == 0 && m_player2 && sharedDualInput() && isGroundMode(m_player2)) {
            g_safeTapJustSent[1] = true;
        }
    }

    void stepPlayer(PlayerObject* p, PState& s, int idx) {
        int step = m_fields->step;
        char const* tag = idx == 0 ? "P1" : "P2";

        if (!s.waveQueue.empty()) {
            if (!p->m_isDart) {
                flushWaveQueue(p, s, idx);
            } else {
                size_t applied = 0;
                while (applied < s.waveQueue.size() && step >= s.waveQueue[applied].second) {
                    auto ev = s.waveQueue[applied];
                    sendInput(p, idx, ev.first, false);
                    DBG("[ClickBuffer] {} WAVE {} {} applied at step {}",
                        tag, waveModeName(), ev.first ? "PRESS" : "RELEASE", step);
                    applied++;
                }
                s.waveQueue.erase(s.waveQueue.begin(), s.waveQueue.begin() + applied);
            }
        }

        if (s.delayedRelease) {
            if (!isFlyMode(p) || step >= s.releaseAt) {
                s.delayedRelease = false;
                s.held = false;
                sendInput(p, idx, false, false);
                DBG("[ClickBuffer] {} MIN-HOLD release at step {} [{}]", tag, step, modeName(p));
            }
        }

        if (s.releaseNext) {
            s.releaseNext = false;
            if (!s.held) sendInput(p, idx, false, true);
        }
        if (g_safeTapJustSent[idx]) {
            g_safeTapJustSent[idx] = false;
            s.releaseNext = true;
        }
        if (!s.pending) return;

        int waited = step - s.pendingStart;
        int limit = modeLimitSteps(p);
        if (limit <= 0 || waited > limit || !isGroundMode(p)) {
            DBG("[ClickBuffer] {} pending expired waited={} limit={} mode={}", tag, waited, limit, modeName(p));
            s.pending = false;
            return;
        }

        int rings = ringsTouching(p);
        if (rings != s.lastRingCount) {
            DBG("[ClickBuffer] {} ring overlap count {} -> {} (waited {})", tag, s.lastRingCount, rings, waited);
            s.lastRingCount = rings;
        }

        bool onGround = p->m_isOnGround;
        bool onRing = rings > 0;
        if (onGround || onRing) {
            s.pending = false;
            float ticksSaved = waited / stepsPerTick();
            char const* what = onRing ? "RING" : "GROUND";
            if (s.held) {
                DBG("[ClickBuffer] {} reached {} after {} steps but button is still held; skipping ({})",
                    tag, what, waited, modeName(p));
            } else {
                sendInput(p, idx, true, true);
                s.releaseNext = true;
                log::info("[ClickBuffer] saved early click +{:.1f} ticks ({:.2f} frames at 60 FPS) [{} {} {}] step {} -> {} limit {} steps",
                          ticksSaved, ticksSaved / 4.0f, tag, modeName(p), what, s.pendingStart, step, limit);
            }
        }
    }

    // Return true when Click Buffer consumes the event; false lets the game
    // process the input normally.
    bool inputPlayer(PlayerObject* p, PState& s, int idx, bool down) {
        int step = m_fields->step;
        char const* tag = idx == 0 ? "P1" : "P2";

        if (!p->m_isDart && !s.waveQueue.empty()) flushWaveQueue(p, s, idx);
        int n = waveGridSteps();
        bool inGrace = step < m_fields->graceUntil;
        if (p->m_isDart && inGrace && n > 0 && g_waveMode != WAVE_HOLD) {
            DBG("[ClickBuffer] {} WAVE {} {} passed through during spawn grace at step {}",
                tag, waveModeName(), down ? "PRESS" : "RELEASE", step);
        }

        if (p->m_isDart && n > 0 && g_waveMode != WAVE_HOLD && !inGrace) {
            int target;
            if (g_waveMode == WAVE_GRID) {
                target = ((step + n - 1) / n) * n;
                if (!s.waveQueue.empty()) {
                    auto const& last = s.waveQueue.back();
                    target = std::max(target, last.second);
                    // A tap must last at least one grid cell or press/release
                    // can collapse into the same physics step.
                    if (!down && last.first) target = std::max(target, last.second + n);
                }
            } else { // WAVE_GAP
                target = step;
                if (!s.waveQueue.empty()) target = std::max(target, s.waveQueue.back().second);
                if (down) {
                    target = std::max(target, s.lastReleaseStep + n);
                } else {
                    s.lastReleaseStep = target;
                }
                if (target == step && s.waveQueue.empty()) {
                    s.held = down;
                    return false;
                }
            }
            s.held = down;
            s.waveQueue.emplace_back(down, target);
            DBG("[ClickBuffer] {} WAVE {} {} queued at step {} -> {} ({} steps)",
                tag, waveModeName(), down ? "PRESS" : "RELEASE", step, target, n);
            return true;
        }

        // Intercept grounded cube clicks. The planner simulates immediate and delayed
        // candidate jumps, then replays the input at the earliest candidate that survives.
        if (!g_safeSimulation && g_safeJumpEnabled && isCubeMode(p)) {
            if (down && p->m_isOnGround && ringsTouching(p) == 0) {
                s.held = true;
                s.pending = false;
                g_safePending[idx] = true;
                g_safePlanReady[idx] = false;
                g_safeDelay[idx] = 0;
                g_safeAge[idx] = 0;
                g_safeNoCandidateNotified[idx] = false;
                DBG("[ClickBuffer] {} captured grounded cube click for safe timing at step {}", tag, step);
                return true;
            }
            if (!down && g_safePending[idx]) {
                s.held = false;
                return true; // replayed tap is independent of physical hold duration
            }
        }

        if (down && s.delayedRelease) {
            s.delayedRelease = false;
            s.held = false;
            sendInput(p, idx, false, false);
        }

        if (!down && isFlyMode(p) && s.pressStep >= 0) {
            int minHold = modeLimitSteps(p);
            int heldFor = step - s.pressStep;
            if (minHold > 0 && heldFor < minHold) {
                s.delayedRelease = true;
                s.releaseAt = s.pressStep + minHold;
                DBG("[ClickBuffer] {} MIN-HOLD delaying release: held {} < {} steps [{}]",
                    tag, heldFor, minHold, modeName(p));
                return true;
            }
        }

        s.held = down;
        if (down) {
            s.pressStep = isFlyMode(p) ? step : -1;
            bool early = isGroundMode(p) && !p->m_isOnGround && ringsTouching(p) == 0;
            if (early && modeLimitSteps(p) > 0) {
                s.pending = true;
                s.pendingStart = step;
                s.lastRingCount = 0;
            } else {
                s.pending = false;
            }
        }
        return false;
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto f = m_fields.operator->();

        // During prediction, leave the mod's real per-player state untouched and
        // only advance the actual game simulation from the temporary checkpoint.
        if (g_safeSimulation) {
            ++g_safeSimElapsed;
            auto target = g_safeSimPlayer == 0 ? m_player1 : m_player2;

            if (g_safeSimFired && g_safeSimReleaseAt == g_safeSimElapsed) {
                if (target) target->releaseButton(PlayerButton::Jump);
                if (g_safeSimPlayer == 0 && m_player2 && sharedDualInput() && isGroundMode(m_player2)) {
                    m_player2->releaseButton(PlayerButton::Jump);
                }
                g_safeSimReleaseAt = -1;
            }
            if (!g_safeSimFired && target && g_safeSimElapsed > g_safeSimCandidateDelay) {
                sendInput(target, g_safeSimPlayer, true, true);
                g_safeSimFired = true;
                g_safeSimReleaseAt = g_safeSimElapsed + 1;
            }

            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }

        f->step++;
        pollKeys();
        syncState();

        // If prediction did not find a survivor, keep the click queued and retry
        // after the world advances. Never force an immediate jump when all tested
        // timings were predicted to die. A queued click is cancelled if the player
        // dies, the level resets, or the cube is no longer grounded.
        for (int idx = 0; idx < 2; ++idx) {
            if (!g_safePending[idx] || g_safePlanReady[idx]) continue;
            auto player = idx == 0 ? m_player1 : m_player2;
            if (!player || player->m_isDead || !player->m_isOnGround) {
                g_safePending[idx] = false;
                g_safePlanReady[idx] = false;
                g_safeDelay[idx] = 0;
                g_safeAge[idx] = 0;
                DBG("[ClickBuffer] cancelled queued safe jump because the player is no longer grounded/alive");
            } else {
                ++g_safeAge[idx];
            }
        }

        // Apply a plan made by the PlayLayer update hook before this tick's physics.
        for (int idx = 0; idx < 2; ++idx) {
            if (!g_safePending[idx] || !g_safePlanReady[idx]) continue;
            auto p = idx == 0 ? m_player1 : m_player2;
            if (!p) {
                g_safePending[idx] = false;
                g_safePlanReady[idx] = false;
                continue;
            }
            if (g_safeDelay[idx] > 0) {
                --g_safeDelay[idx];
            } else {
                fireSafeTap(p, idx);
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (!PlayLayer::get()) {
            for (auto& state : f->ps) state = PState();
            return;
        }

        if (m_player1) stepPlayer(m_player1, f->ps[0], 0);
        if (m_player2) stepPlayer(m_player2, f->ps[1], 1);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto f = m_fields.operator->();
        syncState();
        if (PlayLayer::get() && button == 1) {
            int idx = isPlayer1 ? 0 : 1;
            auto p = isPlayer1 ? m_player1 : m_player2;
            if (p && inputPlayer(p, f->ps[idx], idx, down)) return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};


// Keep the familiar shortcut from the previous Click Buffer popup: hold both
// Control keys to open the new code-drawn settings UI.
namespace {
    bool g_leftControlDown = false;
    bool g_rightControlDown = false;
}

class $modify(ClickBufferKeyboard, CCKeyboardDispatcher) {
    // Geode 4.10.2 / Geometry Dash 2.2074 has the three-argument dispatcher
    // signature. The right-control enum is misspelled KEY_RightContol in that
    // SDK's enumKeyCodes; using the newer names/signature prevents this hook
    // from compiling against the user's target version.
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        bool wasBothDown = g_leftControlDown && g_rightControlDown;
        if (key == KEY_LeftControl) g_leftControlDown = down;
        if (key == KEY_RightContol) g_rightControlDown = down;
        bool bothDown = g_leftControlDown && g_rightControlDown;

        // Open only on the edge where the second Control key is pressed.
        // This avoids repeated attempts while either key is held down.
        bool openedByCombo = down && !repeat && !wasBothDown && bothDown
            && (key == KEY_LeftControl || key == KEY_RightContol);

        if (openedByCombo) {
            auto scene = CCDirector::get()->getRunningScene();
            if (scene && !scene->getChildByID("click-buffer-settings"_spr)) {
                if (auto popup = SafeJumpSettingsPopup::create()) {
                    popup->show();
                }
            }
            // Consume the chord so it doesn't trigger unrelated game input.
            return true;
        }

        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};
