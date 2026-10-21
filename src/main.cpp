#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/Popup.hpp>

using namespace geode::prelude;

namespace {
    float windowSeconds() {
        return Mod::get()->getSettingValue<int64_t>("buffer-window") / 1000.f;
    }

    bool bufferRings() {
        return Mod::get()->getSettingValue<bool>("buffer-rings");
    }

    // Modes where a click only does something on the ground (or on a ring).
    bool isGroundMode(PlayerObject* p) {
        return !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
    }

    bool isLivePlayer(PlayerObject* p) {
        auto pl = PlayLayer::get();
        return pl && (p == pl->m_player1 || p == pl->m_player2) && !p->m_isDead;
    }

    bool touchingRing(PlayerObject* p) {
        return p->m_touchingRings && p->m_touchingRings->count() > 0;
    }
}

class $modify(BufferedPlayer, PlayerObject) {
    struct Fields {
        bool pending = false;
        bool userHolding = false;
        bool injecting = false;
        bool releaseNext = false;
        float timeLeft = 0.f;
    };

    bool pushButton(PlayerButton btn) {
        auto& f = *m_fields.operator->();
        if (btn != PlayerButton::Jump || f.injecting) {
            return PlayerObject::pushButton(btn);
        }

        bool shouldBuffer = isLivePlayer(this)
            && isGroundMode(this)
            && !m_isOnGround
            && !touchingRing(this);

        f.userHolding = true;
        bool ret = PlayerObject::pushButton(btn);

        if (shouldBuffer) {
            f.pending = true;
            f.timeLeft = windowSeconds();
        }
        return ret;
    }

    bool releaseButton(PlayerButton btn) {
        if (btn == PlayerButton::Jump && !m_fields->injecting) {
            m_fields->userHolding = false;
        }
        return PlayerObject::releaseButton(btn);
    }

    void update(float dt) {
        auto& f = *m_fields.operator->();

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
                f.timeLeft -= dt;
                if (f.timeLeft <= 0.f) {
                    f.pending = false;
                } else if (m_isOnGround || (bufferRings() && touchingRing(this))) {
                    f.pending = false;
                    f.injecting = true;
                    if (f.userHolding) PlayerObject::releaseButton(PlayerButton::Jump);
                    PlayerObject::pushButton(PlayerButton::Jump);
                    f.injecting = false;
                    f.releaseNext = !f.userHolding;
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

        if (Mod::get()->getSettingValue<bool>("safe-mode")) {
            m_isTestMode = true;
        }
        return true;
    }
};

class BufferMenu : public geode::Popup {
protected:
    CCLabelBMFont* m_windowLabel = nullptr;

    bool init() {
        if (!Popup::init(300.f, 190.f)) return false;
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
        int64_t v = Mod::get()->getSettingValue<int64_t>("buffer-window") + delta;
        v = std::clamp<int64_t>(v, 10, 250);
        Mod::get()->setSettingValue<int64_t>("buffer-window", v);
        this->refreshWindowLabel();
    }

    void onWindowDown(CCObject*) { adjustWindow(-10); }
    void onWindowUp(CCObject*) { adjustWindow(10); }

    void refreshWindowLabel() {
        auto v = Mod::get()->getSettingValue<int64_t>("buffer-window");
        m_windowLabel->setString(fmt::format("Window: {} ms", v).c_str());
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
