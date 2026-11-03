#pragma once

#include "esphome/core/defines.h"

#ifdef USE_NSPANEL_SCREEN_LIGHT

#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "nspanel_lovelace.h"

namespace esphome {
namespace nspanel_lovelace {

// Normally the brightness sets the screen's normal brightness and off is the same as
// 'Turn screen off'. For the screensaver it sets the screensaver brightness and off is 0.
class ScreenLight : public light::LightOutput {
public:
  void set_parent(NSPanelLovelace *parent) { this->parent_ = parent; }
  void set_screensaver(bool screensaver) { this->screensaver_ = screensaver; }
  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
    return traits;
  }
  void setup_state(light::LightState *state) override {
    if (this->screensaver_) this->parent_->set_screensaver_light_state(state);
    else this->parent_->set_screen_light_state(state);
  }
  void write_state(light::LightState *state) override {
    bool on = state->current_values.is_on();
    float brightness = state->current_values.get_brightness();
    if (this->screensaver_) this->parent_->set_screensaver_from_light(on, brightness);
    else this->parent_->set_screen_from_light(on, brightness);
  }

protected:
  NSPanelLovelace *parent_{nullptr};
  bool screensaver_{false};
};

} // namespace nspanel_lovelace
} // namespace esphome

#endif // USE_NSPANEL_SCREEN_LIGHT
