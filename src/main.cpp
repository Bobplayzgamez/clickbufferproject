#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/Popup.hpp>

using namespace geode::prelude;

namespace {
    int bufferFrames() {
        return (int)std::clamp<int64_t>(Mod::get()->getSettingValue<int64_t>("buffer-frames"), 1, 10);
    }

    bool bufferRings() {
        return Mod::get()->getSettingValue<bool>("buffer-rings");
    }

    bool lookaheadOn() {
        return Mod::get()->getSettingValue<bool>("lookahead");
    }

    int lookaheadSteps() {
        return (int)std::clamp<int64_t>(Mod::get()->getSettingValue<int64_t>("lookahead-steps"), 20, 200);
    }

    // Modes where a click only does something on the ground (or on a ring).
    bool isGroundMode(PlayerObject* p) {
        return !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
    }

    bool touchingRing(PlayerObject* p) {
        return p->m_touchingRings && p->m_touchingRings->count() > 0;
    }

    // Per-player buffer state. Index 0 = player 1, index 1 = player 2.
    struct BufState {
        bool pending = false;      // a click is waiting to be fired
        bool userHolding = false;  // is the real input currently held?
        bool injecting = false;    // guard so our own calls don't re-trigger the hooks
        bool releaseNext = false;  // we need to let go of a synthetic tap
        int framesLeft = 0;        // physics steps left in the buffer window
        int notBefore = 0;         // lookahead: do not fire before this many steps have passed
        bool awaitPlan = false;    // lookahead: hold the click until the plan is made
    };
    BufState g_buf[2];

    // Lookahead simulation flags
    bool g_sim = false;       // true while the mod is simulating future steps
    bool g_simDied = false;   // the simulated player died
    bool g_simFired = false;  // the simulated run fired the buffered jump
    int g_stepCount = 0;      // counts player-1 physics steps

    int playerIndex(PlayerObject* p) {
        auto pl = PlayLayer::get();
        if (!pl) return -1;
        if (p == pl->m_player1) return 0;
        if (p == pl->m_player2) return 1;
        return -1;
    }
}

class $modify(BufferedPlayer, PlayerObject) {
    bool pushButton(PlayerButton btn) {
        int idx = playerIndex(this);
        if (idx < 0 || btn != PlayerButton::Jump || g_buf[idx].injecting) {
            return PlayerObject::pushButton(btn);
        }
        auto& f = g_buf[idx];

        // Lookahead: hold ground/ring clicks too, so the plan can pick the exact frame.
        if (lookaheadOn()
            && !g_sim
            && !m_isDead
            && isGroundMode(this)
            && (m_isOnGround || (bufferRings() && touchingRing(this)))) {
            f.userHolding = true;
            f.pending = true;
            f.awaitPlan = true;
            f.framesLeft = bufferFrames();
            f.notBefore = 0;
            return true; // the real jump is fired later by update()
        }

        // Decide BEFORE calling the original, since a ground jump changes m_isOnGround.
        bool shouldBuffer = !g_sim
            && !m_isDead
            && isGroundMode(this)
            && !m_isOnGround
            && !touchingRing(this);

        f.userHolding = true;
        bool ret = PlayerObject::pushButton(btn);

        if (shouldBuffer) {
            f.pending = true;
            f.framesLeft = bufferFrames();
            f.notBefore = 0;
            f.awaitPlan = false;
        }
        return ret;
    }

    bool releaseButton(PlayerButton btn) {
        int idx = playerIndex(this);
        if (idx >= 0 && btn == PlayerButton::Jump && !g_buf[idx].injecting) {
            g_buf[idx].userHolding = false;
        }
        return PlayerObject::releaseButton(btn);
    }

    // Runs once per physics step.
    void update(float dt) {
        int idx = playerIndex(this);
        if (idx < 0) {
            PlayerObject::update(dt);
            return;
        }
        auto& f = g_buf[idx];
        if (idx == 0) g_stepCount++;

        // Finish a synthetic tap from the previous step.
        if (f.releaseNext) {
            f.releaseNext = false;
            if (!f.userHolding) {
                f.injecting = true;
                PlayerObject::releaseButton(PlayerButton::Jump);
                f.injecting = false;
            }
        }

        if (f.pending) {
            if (m_isDead || !isGroundMode(this)) {
                f.pending = false;
            } else {
                f.framesLeft--;
                if (f.framesLeft < 0) {
                    f.pending = false; // buffer expired
                } else if (f.awaitPlan) {
                    // waiting for the lookahead plan; do nothing yet
                } else if (f.notBefore > 0) {
                    f.notBefore--;     // lookahead says: wait
                } else if (m_isOnGround || (bufferRings() && touchingRing(this))) {
                    f.pending = false;
                    f.injecting = true;
                    // Fresh click: if the user is still holding, release first so the
                    // game sees a real press edge.
                    if (f.userHolding) PlayerObject::releaseButton(PlayerButton::Jump);
                    PlayerObject::pushButton(PlayerButton::Jump);
                    f.injecting = false;
                    f.releaseNext = !f.userHolding;
                    if (g_sim) g_simFired = true;
                }
            }
        }

        if (m_isDead) f.releaseNext = false;

        PlayerObject::update(dt);
    }
};

class $modify(BufferPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        g_buf[0] = BufState();
        g_buf[1] = BufState();

        if (Mod::get()->getSettingValue<bool>("safe-mode")) {
            m_isTestMode = true; // prevents progress/stat saving
        }
        return true;
    }

    // While simulating, deaths are only recorded, never actually applied.
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_sim) {
            if (object != m_anticheatSpike) g_simDied = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        if (g_sim) return; // never finish the level from inside a simulation
        PlayLayer::levelComplete();
    }

    // Advance the game by exactly one physics step (for the simulation).
    void simStep() {
        int before = g_stepCount;
        for (int i = 0; i < 4 && g_stepCount == before; ++i) {
            PlayLayer::update(1.f / 240.f);
        }
    }

    // Try each possible fire time, simulate ahead, and pick the first one that survives.
    void planJump() {
        auto cp = this->createCheckpoint();
        if (!cp) return;
        cp->retain();

        BufState saved0 = g_buf[0];
        BufState saved1 = g_buf[1];
        int steps = lookaheadSteps();
        int best = 0; // fall back to the normal behavior if nothing works

        int window = 0;
        if (saved0.pending) window = std::max(window, saved0.framesLeft);
        if (saved1.pending) window = std::max(window, saved1.framesLeft);

        g_sim = true;
        for (int c = 0; c < window; ++c) {
            g_buf[0] = saved0;
            g_buf[1] = saved1;
            if (saved0.pending) { g_buf[0].notBefore = c; g_buf[0].awaitPlan = false; }
            if (saved1.pending) { g_buf[1].notBefore = c; g_buf[1].awaitPlan = false; }
            g_simDied = false;
            g_simFired = false;

            for (int s = 0; s < steps && !g_simDied; ++s) {
                this->simStep();
            }

            bool ok = !g_simDied && g_simFired;
            this->loadFromCheckpoint(cp);
            if (ok) {
                best = c;
                break;
            }
        }
        g_sim = false;

        g_buf[0] = saved0;
        g_buf[1] = saved1;
        if (saved0.pending) { g_buf[0].notBefore = best; g_buf[0].awaitPlan = false; }
        if (saved1.pending) { g_buf[1].notBefore = best; g_buf[1].awaitPlan = false; }
        cp->release();
    }

    void update(float dt) {
        if (!g_sim
            && lookaheadOn()
            && (g_buf[0].pending || g_buf[1].pending)
            && m_player1
            && !m_player1->m_isDead) {
            this->planJump();
        }
        PlayLayer::update(dt);
    }
};

class BufferMenu : public geode::Popup {
protected:
    CCLabelBMFont* m_windowLabel = nullptr;

    bool init() {
        if (!Popup::init(300.f, 225.f)) return false;
        this->setTitle("Click Buffer");

        auto size = m_mainLayer->getContentSize();
        float cx = size.width / 2.f;

        m_windowLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_windowLabel->setScale(0.5f);
        m_windowLabel->setPosition({cx, size.height - 65.f});
        m_mainLayer->addChild(m_windowLabel);
        this->refreshWindowLabel();

        auto minus = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("-", 30, true, "goldFont.fnt", "GJ_button_01.png", 30.f, 0.8f),
            this, menu_selector(BufferMenu::onWindowDown)
        );
        minus->setPosition({cx - 90.f, size.height - 65.f});
        m_buttonMenu->addChild(minus);

        auto plus = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("+", 30, true, "goldFont.fnt", "GJ_button_01.png", 30.f, 0.8f),
            this, menu_selector(BufferMenu::onWindowUp)
        );
        plus->setPosition({cx + 90.f, size.height - 65.f});
        m_buttonMenu->addChild(plus);

        this->addToggle("Buffer Rings", "buffer-rings", size.height - 105.f);
        this->addToggle("Safe Mode", "safe-mode", size.height - 140.f);
        this->addToggle("Lookahead (beta)", "lookahead", size.height - 175.f);

        return true;
    }

    void addToggle(char const* text, char const* settingId, float y) {
        auto size = m_mainLayer->getContentSize();

        auto toggler = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(BufferMenu::onToggle), 0.7f
        );
        toggler->setPosition({size.width / 2.f - 70.f, y});
        toggler->setUserObject("setting-id", CCString::create(settingId));
        toggler->toggle(Mod::get()->getSettingValue<bool>(settingId));
        m_buttonMenu->addChild(toggler);

        auto label = CCLabelBMFont::create(text, "bigFont.fnt");
        label->setScale(0.45f);
        label->setAnchorPoint({0.f, 0.5f});
        label->setPosition({size.width / 2.f - 45.f, y});
        m_mainLayer->addChild(label);
    }

    void onToggle(CCObject* sender) {
        auto toggler = static_cast<CCMenuItemToggler*>(sender);
        auto id = static_cast<CCString*>(toggler->getUserObject("setting-id"));
        // Callback fires before the toggler flips, so the new value is the inverse.
        Mod::get()->setSettingValue<bool>(id->getCString(), !toggler->isToggled());
    }

    void adjustWindow(int delta) {
        int64_t v = Mod::get()->getSettingValue<int64_t>("buffer-frames") + delta;
        v = std::clamp<int64_t>(v, 1, 10);
        Mod::get()->setSettingValue<int64_t>("buffer-frames", v);
        this->refreshWindowLabel();
    }

    void onWindowDown(CCObject*) { adjustWindow(-1); }
    void onWindowUp(CCObject*) { adjustWindow(1); }

    void refreshWindowLabel() {
        auto v = Mod::get()->getSettingValue<int64_t>("buffer-frames");
        m_windowLabel->setString(fmt::format("Buffer: {} frames", v).c_str());
    }

public:
    static BufferMenu* create() {
        auto ret = new BufferMenu();
        if (ret->init()) {
            ret->setID("menu"_spr);
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

namespace {
    bool g_leftCtrl = false;
    bool g_rightCtrl = false;
}

class $modify(ComboKeyDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (key == KEY_LeftControl)  g_leftCtrl = down;
        if (key == KEY_RightControl) g_rightCtrl = down;

        bool comboJustPressed = down && !repeat
            && (key == KEY_LeftControl || key == KEY_RightControl)
            && g_leftCtrl && g_rightCtrl;

        if (comboJustPressed) {
            auto scene = CCDirector::get()->getRunningScene();
            if (scene && !scene->getChildByID("menu"_spr)) {
                BufferMenu::create()->show();
            }
            return true;
        }

        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};
