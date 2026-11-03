#include "nspanel_lovelace.h"

#ifdef USE_NSPANEL_PANEL_SETTINGS

#include "cards.h"
#include "card_items.h"
#include "translations.h"
#include "types.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/wifi/wifi_component.h"
#include <esp_timer.h>
#include <algorithm>
#include <nvs.h>

namespace esphome {
namespace nspanel_lovelace {

static const char *const TAG = "nspanel_lovelace.settings";

static const char *const SETTINGS_NVS_NAMESPACE = "nspanel";
static const char *const SETTINGS_NVS_KEY = "settings";
static const char *const LOCK_ENTITY_ID = "nspanel.panel_lock";
static const char *const LOCK_CARD_UUID = "panel_lock";
static const char *const SETTINGS_CARD_UUID = "panel_settings";
static const char *const LOCK_TITLE_TIMEOUT = "lock_title";
static const uint32_t LOCK_MESSAGE_MS = 3000;
static const char *const BRIGHTNESS_PREVIEW_TIMEOUT = "dim_preview";
static const uint32_t BRIGHTNESS_PREVIEW_MS = 1000;
static const uint8_t LOCK_MAX_ATTEMPTS = 5;
static const uint32_t LOCK_COOLDOWN_MS = 30000;
static const uint32_t REBOOT_DELAY_MS = 200;
static const uint8_t BRIGHTNESS_MIN = 5;
static const char *const NOTIFY_ACTION = "notifyAction";

static const uint8_t ACCESS_OPEN = 0;
static const uint8_t ACCESS_PIN = 1;
static const uint8_t ACCESS_HIDDEN = 2;

static const uint8_t LOCK_DISPLAY_KEYPAD = 0;
static const uint8_t LOCK_DISPLAY_SCREENSAVER = 1;

static const char *const DIAGNOSTICS_CARD_UUID = "panel_diagnostics";
static const char *const DIAGNOSTICS_INTERVAL = "diagnostics";
static const uint32_t DIAGNOSTICS_INTERVAL_MS = 5000;

static const icon_char_t *const ICON_BRIGHTNESS = u8"\uE0DE";
static const icon_char_t *const ICON_SCREENSAVER_BRIGHTNESS = u8"\uE0DD";
static const icon_char_t *const ICON_SCREEN_OFF = u8"\uED8F";
static const icon_char_t *const ICON_COG = u8"";
static const icon_char_t *const ICON_RESTART = u8"\uE708";
static const icon_char_t *const ICON_INFORMATION = u8"\uE2FB";
static const icon_char_t *const ICON_WIFI = u8"\uE5A8";
static const icon_char_t *const ICON_IP = u8"\uEA5F";
static const icon_char_t *const ICON_HOME_ASSISTANT = u8"\uE7CF";
static const icon_char_t *const ICON_UPTIME = u8"\uE51A";
static const icon_char_t *const ICON_THERMOMETER = u8"\uE50E";

struct PanelSettingsState {
  uint8_t locked;
  uint8_t buttons_locked;
  uint8_t access;
  uint8_t lock_display;
  uint32_t pin_hash;
};

static uint32_t hash_pin(const std::string &pin) {
  // Same on every panel, so that Home Assistant can give them all one PIN without keeping it
  uint32_t hash = fnv1_hash(std::string("nspanel-pin:") + pin);
  return hash != 0 ? hash : 1;
}

static bool is_valid_pin(const std::string &pin) {
  if (pin.size() < 4 || pin.size() > 8) return false;
  for (char c : pin) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

static std::unique_ptr<NavigationItem> make_nav(const std::string &target, const icon_char_t *icon) {
  static uint16_t index = 0;
  return std::unique_ptr<NavigationItem>(
      new NavigationItem(std::string("sn") + std::to_string(++index), target, icon));
}

static std::string format_uptime() {
  auto seconds = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
  char buffer[24];
  if (seconds >= 86400) snprintf(buffer, sizeof(buffer), "%" PRIu32 "d %" PRIu32 "h", seconds / 86400, seconds % 86400 / 3600);
  else if (seconds >= 3600) snprintf(buffer, sizeof(buffer), "%" PRIu32 "h %" PRIu32 "m", seconds / 3600, seconds % 3600 / 60);
  else snprintf(buffer, sizeof(buffer), "%" PRIu32 "m %" PRIu32 "s", seconds / 60, seconds % 60);
  return buffer;
}

static void set_nav_left(Card *card, std::unique_ptr<NavigationItem> nav) { card->set_nav_left(nav); }
static void set_nav_right(Card *card, std::unique_ptr<NavigationItem> nav) { card->set_nav_right(nav); }

void NSPanelLovelace::setup_panel_settings_() {
  nvs_handle_t handle;
  if (nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
    PanelSettingsState state{};
    size_t size = sizeof(state);
    if (nvs_get_blob(handle, SETTINGS_NVS_KEY, &state, &size) == ESP_OK && size == sizeof(state)) {
      this->lock_pin_hash_ = state.pin_hash;
      this->panel_locked_ = state.locked != 0 && state.pin_hash != 0;
      this->buttons_locked_ = state.buttons_locked != 0;
      this->settings_access_ = state.access;
      this->lock_shows_screensaver_ = state.lock_display == LOCK_DISPLAY_SCREENSAVER;
    }
    nvs_close(handle);
  }

  uint16_t sleep_timeout = 10;
  for (auto &page : this->page_mgr_) {
    if (!page->is_type(page_type::screensaver)) {
      sleep_timeout = page->get_sleep_timeout();
      break;
    }
  }

  this->lock_card_ = this->create_page<AlarmCard>(LOCK_CARD_UUID,
      this->create_entity(LOCK_ENTITY_ID), get_translation("panel_locked"), sleep_timeout);
  this->lock_card_->set_hidden(true);
  this->lock_card_->set_disarm_text("OK");
  this->lock_sleep_timeout_ = sleep_timeout;

  auto settings = this->create_page<EntitiesCard>(
      SETTINGS_CARD_UUID, get_translation("panel_settings"), sleep_timeout);
  settings->set_hidden(true);
  this->settings_card_ = settings;

  auto buttons_row = std::make_shared<EntitiesCardEntityItem>("sr1",
      this->create_entity("nspanel.button_lock"), std::string(get_translation("panel_lock_buttons")));
  buttons_row->set_icon_value(icon_t::gesture_tap_button);
  settings->add_item(buttons_row);
  auto brightness_row = std::make_shared<EntitiesCardEntityItem>("sr4",
      this->create_entity("nspanel.brightness"), std::string(get_translation(translation_item::brightness)));
  brightness_row->set_icon_value(ICON_BRIGHTNESS);
  settings->add_item(brightness_row);
  auto screensaver_brightness_row = std::make_shared<EntitiesCardEntityItem>("sr6",
      this->create_entity("nspanel.screensaver_brightness"),
      std::string(get_translation("panel_screensaver_brightness")));
  screensaver_brightness_row->set_icon_value(ICON_SCREENSAVER_BRIGHTNESS);
  settings->add_item(screensaver_brightness_row);
  // Without a screensaver there is nothing that a tap can wake the screen from
  for (auto &page : this->page_mgr_) {
    if (page->is_type(page_type::screensaver)) this->has_screensaver_ = true;
  }
  // Models with 4 rows would otherwise lose the lock row
  if (this->has_screensaver_ && this->settings_max_rows_ > 4) {
    auto screen_off_row = std::make_shared<EntitiesCardEntityItem>("sr5",
        this->create_entity("nspanel.screen_off"), std::string(get_translation("panel_screen_off")));
    screen_off_row->set_icon_value(ICON_SCREEN_OFF);
    settings->add_item(screen_off_row);
  }
  auto lock_row = std::make_shared<EntitiesCardEntityItem>("sr3",
      this->create_entity("nspanel.lock"), std::string(get_translation("panel_lock_screen")));
  lock_row->set_icon_value(icon_t::lock);
  this->lock_row_ = lock_row;

  this->setup_diagnostics_(sleep_timeout);
  this->update_lock_row_();
  this->update_lock_card_();
  this->update_panel_entities_();
  this->link_settings_page_();
#ifdef USE_NSPANEL_SCREEN_LIGHT
  this->set_timeout(1000, [this]() {
    this->screen_light_ready_ = true;
    this->sync_screen_light_();
  });
#endif

  this->register_service(&NSPanelLovelace::lock_panel, "lock_panel");
  this->register_service(&NSPanelLovelace::unlock_panel, "unlock_panel");
  this->register_service(&NSPanelLovelace::set_lock_pin, "set_lock_pin", {"pin"});
  this->register_service(&NSPanelLovelace::set_lock_pin_hash, "set_lock_pin_hash", {"hash"});
  this->register_service(&NSPanelLovelace::set_button_lock, "set_button_lock", {"locked"});
  this->register_service(&NSPanelLovelace::set_settings_access, "set_settings_access", {"access"});
  this->register_service(&NSPanelLovelace::set_lock_display, "set_lock_display", {"display"});
}

void NSPanelLovelace::setup_diagnostics_(uint16_t sleep_timeout) {
  auto diagnostics = this->create_page<EntitiesCard>(
      DIAGNOSTICS_CARD_UUID, get_translation("panel_diagnostics"), sleep_timeout);
  diagnostics->set_hidden(true);
  this->diagnostics_card_ = diagnostics;

  // The reboot row always fits, the last of the others make room for it on smaller screens
  uint8_t rows = 0;
  auto add_row = [&](const char *row_id, const char *entity_id, const char *name, const icon_char_t *icon) {
    if (rows + 1 >= this->settings_max_rows_) return;
    auto row = std::make_shared<EntitiesCardEntityItem>(row_id,
        this->create_entity(entity_id), std::string(get_translation(name)));
    row->set_icon_value(icon);
    diagnostics->add_item(row);
    rows++;
  };
  add_row("dr1", "nspanel.diag_wifi", "panel_wifi", ICON_WIFI);
  add_row("dr2", "nspanel.diag_ip", "panel_ip_address", ICON_IP);
  add_row("dr3", "nspanel.diag_home_assistant", "panel_home_assistant", ICON_HOME_ASSISTANT);
  add_row("dr4", "nspanel.diag_uptime", "panel_uptime", ICON_UPTIME);
#ifdef USE_NSPANEL_TEMPERATURE_SENSOR
  if (this->temperature_sensor_ != nullptr) {
    add_row("dr5", "nspanel.diag_temperature", "panel_temperature", ICON_THERMOMETER);
  }
#endif
  auto reboot_row = std::make_shared<EntitiesCardEntityItem>("dr6",
      this->create_entity("nspanel.reboot"), std::string(get_translation("panel_reboot")));
  reboot_row->set_icon_value(ICON_RESTART);
  diagnostics->add_item(reboot_row);

  // Only worth doing while someone is looking at it
  this->set_interval(DIAGNOSTICS_INTERVAL, DIAGNOSTICS_INTERVAL_MS, [this]() {
    if (this->page_mgr_.current_page() != this->diagnostics_card_) return;
    this->update_panel_entities_();
    this->render_item_update_();
  });
}

std::string NSPanelLovelace::diagnostics_value_(const std::string &entity_id) const {
  const char *disconnected = get_translation("panel_disconnected");
  auto *wifi_component = wifi::global_wifi_component;
  if (entity_id == "nspanel.diag_wifi") {
    if (!wifi_component->is_connected()) return disconnected;
    return esphome::to_string(wifi_component->wifi_rssi()) + " dBm";
  }
  if (entity_id == "nspanel.diag_ip") {
    if (!wifi_component->is_connected()) return "--";
    for (auto &address : wifi_component->wifi_sta_ip_addresses()) {
      if (address.is_set()) return address.str();
    }
    return "--";
  }
  if (entity_id == "nspanel.diag_home_assistant") {
    return this->is_connected() ? get_translation("panel_connected") : disconnected;
  }
  if (entity_id == "nspanel.diag_uptime") return format_uptime();
#ifdef USE_NSPANEL_TEMPERATURE_SENSOR
  if (entity_id == "nspanel.diag_temperature") {
    if (this->temperature_sensor_ == nullptr || std::isnan(this->temperature_sensor_->get_state())) return "--";
    return value_accuracy_to_string(this->temperature_sensor_->get_state(), 1) + " °C";
  }
#endif
  return "";
}

void NSPanelLovelace::save_panel_settings_() {
  PanelSettingsState state{};
  state.locked = this->panel_locked_ ? 1 : 0;
  state.buttons_locked = this->buttons_locked_ ? 1 : 0;
  state.access = this->settings_access_;
  state.lock_display = this->lock_shows_screensaver_ ? LOCK_DISPLAY_SCREENSAVER : LOCK_DISPLAY_KEYPAD;
  state.pin_hash = this->lock_pin_hash_;

  nvs_handle_t handle;
  esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    err = nvs_set_blob(handle, SETTINGS_NVS_KEY, &state, sizeof(state));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to store the settings: %s", esp_err_to_name(err));
  }
}

void NSPanelLovelace::update_panel_entities_() {
  for (auto &entity : this->entities_) {
    auto &entity_id = entity->get_entity_id();
    if (get_entity_type(entity_id) != entity_type::nspanel) continue;
    if (entity_id == LOCK_ENTITY_ID) {
      // the keypad and the unlock button only show on an armed alarm card
      entity->set_state(entity_state::armed_away);
    } else if (entity_id == "nspanel.button_lock") {
      entity->set_state(this->buttons_locked_ ? entity_state::on : entity_state::off);
    } else if (entity_id == "nspanel.brightness") {
      entity->set_attribute(ha_attr_type::min, esphome::to_string(BRIGHTNESS_MIN));
      entity->set_state(esphome::to_string(this->display_active_dim_));
    } else if (entity_id == "nspanel.screensaver_brightness") {
      entity->set_attribute(ha_attr_type::min, "0");
      entity->set_state(esphome::to_string(this->display_inactive_dim_));
    } else if (entity_id.rfind("nspanel.diag_", 0) == 0) {
      entity->set_state(this->diagnostics_value_(entity_id));
    } else {
      entity->set_state(entity_state::idle);
    }
  }
}

void NSPanelLovelace::report_panel_settings_() {
  this->update_panel_entities_();
#ifdef USE_NSPANEL_RUNTIME_LAYOUT
  if (this->is_connected()) this->report_layout_status();
#endif
}

// Rewrites the arrows at the ends of the page rotation, so this works for pages from
// the device configuration and for runtime layouts. There are only two nav slots: with
// a single page the gear takes the free right slot, otherwise the arrows at the ends of
// the rotation lead to settings. Settings itself uses its slots for home and diagnostics.
void NSPanelLovelace::link_settings_page_() {
  std::vector<Card *> cards;
  for (auto &page : this->page_mgr_) {
    if (page->is_hidden() || page->is_type(page_type::screensaver)) continue;
    cards.push_back(static_cast<Card *>(page.get()));
  }
  if (cards.empty()) return;

  const std::string &uuid = this->settings_card_->get_uuid();
  bool show = this->settings_access_ != ACCESS_HIDDEN;
  Card *first = cards.front();
  Card *last = cards.back();
  if (cards.size() == 1) {
    set_nav_right(first, show ? make_nav(uuid, ICON_COG) : nullptr);
  } else {
    set_nav_left(first, make_nav(show ? uuid : last->get_uuid(), icon_t::arrow_left_bold));
    set_nav_right(last, make_nav(show ? uuid : first->get_uuid(), icon_t::arrow_right_bold));
  }
  set_nav_left(this->settings_card_, make_nav(first->get_uuid(), icon_t::home));
  set_nav_right(this->settings_card_, make_nav(this->diagnostics_card_->get_uuid(), ICON_INFORMATION));
  set_nav_left(this->diagnostics_card_, make_nav(uuid, icon_t::arrow_left_bold));
}

void NSPanelLovelace::update_lock_row_() {
  this->settings_card_->remove_item(this->lock_row_->get_uuid());
  if (this->lock_pin_hash_ != 0) this->settings_card_->add_item(this->lock_row_);
}

void NSPanelLovelace::update_lock_card_() {
  // A timeout of 0 keeps the keypad on screen, there is no screensaver to go to
  bool keep_on_screen = this->panel_locked_ && !this->lock_shows_screensaver_;
  this->lock_card_->set_sleep_timeout(keep_on_screen ? 0 : this->lock_sleep_timeout_);
  this->lock_card_->set_title(this->lock_title_());
}

const char *NSPanelLovelace::lock_title_() const {
  return get_translation(this->panel_locked_ ? "panel_locked" : "panel_settings");
}

bool NSPanelLovelace::process_settings_nav_(const std::string &uuid) {
  if (uuid == this->diagnostics_card_->get_uuid()) {
    this->update_panel_entities_();
    return false;
  }
  if (uuid != this->settings_card_->get_uuid()) return false;
  // Coming back from diagnostics, the PIN was already given
  if (this->page_mgr_.current_page() == this->diagnostics_card_) return false;
  if (this->settings_access_ != ACCESS_PIN || this->lock_pin_hash_ == 0) return false;
  this->cancel_timeout(LOCK_TITLE_TIMEOUT);
  this->update_lock_card_();
  auto current = this->page_mgr_.current_page();
  if (current != nullptr) {
    set_nav_left(this->lock_card_, make_nav(current->get_uuid(), icon_t::arrow_left_bold));
  }
  this->force_current_page_update_ = true;
  this->render_page_(this->lock_card_->get_uuid());
  return true;
}

void NSPanelLovelace::wake_screen_(bool sync_light) {
  if (!this->screen_off_) return;
  this->screen_off_ = false;
  this->set_display_dim();
  if (sync_light) this->sync_screen_light_();
}

void NSPanelLovelace::turn_screen_off_() {
  if (!this->has_screensaver_) {
    this->sync_screen_light_();
    return;
  }
  if (this->screen_off_) return;
  this->screen_off_ = true;
  this->set_display_dim();
  this->force_current_page_update_ = true;
  this->render_page_(render_page_option::screensaver_page);
  this->sync_screen_light_();
}

void NSPanelLovelace::on_brightness_changed_() {
  this->update_panel_entities_();
  this->sync_screen_light_();
}

void NSPanelLovelace::sync_screen_light_() {
#ifdef USE_NSPANEL_SCREEN_LIGHT
  if (!this->screen_light_ready_) return;
  if (this->screen_light_ != nullptr) {
    auto call = this->screen_light_->make_call();
    call.set_state(!this->screen_off_);
    call.set_brightness(this->display_active_dim_ / 100.0f);
    call.set_transition_length(0);
    call.set_save(false);
    call.perform();
  }
  if (this->screensaver_light_ != nullptr) {
    // Off is a brightness of 0, the light keeps its last value for turning back on
    auto call = this->screensaver_light_->make_call();
    call.set_state(this->display_inactive_dim_ > 0);
    if (this->display_inactive_dim_ > 0) call.set_brightness(this->display_inactive_dim_ / 100.0f);
    call.set_transition_length(0);
    call.set_save(false);
    call.perform();
  }
#endif
}

#ifdef USE_NSPANEL_SCREEN_LIGHT
void NSPanelLovelace::set_screensaver_from_light(bool on, float brightness) {
  if (!this->screen_light_ready_) return;
  uint8_t inactive = on ? std::min<int>(std::max<int>(brightness * 100.0f + 0.5f, 1), 100) : 0;
  if (inactive != this->display_inactive_dim_) {
    this->set_display_dim(inactive, this->display_active_dim_);
    this->update_panel_entities_();
  }
}

void NSPanelLovelace::set_screen_from_light(bool on, float brightness) {
  // The light starts on at full brightness, the panel's own state must win over that
  if (!this->screen_light_ready_) return;
  if (!on) {
    this->turn_screen_off_();
    return;
  }
  this->wake_screen_(false);
  int value = static_cast<int>(brightness * 100.0f + 0.5f);
  uint8_t active = std::min<int>(std::max<int>(value, BRIGHTNESS_MIN), 100);
  if (active != this->display_active_dim_) {
    this->set_display_dim(this->display_inactive_dim_, active);
    this->update_panel_entities_();
  }
}
#endif

void NSPanelLovelace::lock_panel() {
  if (this->lock_pin_hash_ == 0) {
    ESP_LOGW(TAG, "Cannot lock the panel, no PIN is set");
    return;
  }
  this->panel_locked_ = true;
  this->lock_failed_attempts_ = 0;
  this->cancel_timeout(LOCK_TITLE_TIMEOUT);
  this->update_lock_card_();
  set_nav_left(this->lock_card_, nullptr);
  this->save_panel_settings_();
  this->report_panel_settings_();
  if (this->lock_shows_screensaver_) {
    this->force_current_page_update_ = true;
    this->render_page_(render_page_option::screensaver_page);
  } else {
    this->render_current_page_();
  }
}

void NSPanelLovelace::unlock_panel() {
  if (!this->panel_locked_) return;
  this->panel_locked_ = false;
  this->lock_failed_attempts_ = 0;
  this->cancel_timeout(LOCK_TITLE_TIMEOUT);
  this->update_lock_card_();
  this->save_panel_settings_();
  this->report_panel_settings_();
  if (this->page_mgr_.current_page() == this->lock_card_) {
    this->force_current_page_update_ = true;
    this->render_page_(render_page_option::default_page);
  }
}

void NSPanelLovelace::set_lock_pin(std::string pin) {
  if (!pin.empty() && !is_valid_pin(pin)) {
    ESP_LOGW(TAG, "Ignoring PIN, it must have 4 to 8 digits");
    return;
  }
  this->apply_lock_pin_hash_(pin.empty() ? 0 : hash_pin(pin));
}

void NSPanelLovelace::set_lock_pin_hash(std::string hash) {
  if (hash.size() != 8) {
    ESP_LOGW(TAG, "Ignoring PIN hash, it must have 8 hex characters");
    return;
  }
  auto value = parse_hex<uint32_t>(hash);
  if (!value.has_value()) {
    ESP_LOGW(TAG, "Ignoring PIN hash, it must have 8 hex characters");
    return;
  }
  // Home Assistant sends this every time the panel reports, so don't report back or write to flash
  if (*value == this->lock_pin_hash_) return;
  this->apply_lock_pin_hash_(*value);
}

void NSPanelLovelace::apply_lock_pin_hash_(uint32_t hash) {
  this->lock_pin_hash_ = hash;
  this->update_lock_row_();
  this->save_panel_settings_();
  if (hash == 0 && this->panel_locked_) {
    this->unlock_panel();
  } else {
    this->report_panel_settings_();
  }
}

void NSPanelLovelace::set_button_lock(bool locked) {
  this->buttons_locked_ = locked;
  this->save_panel_settings_();
  this->report_panel_settings_();
}

void NSPanelLovelace::set_settings_access(std::string access) {
  if (access == "open") this->settings_access_ = ACCESS_OPEN;
  else if (access == "pin") this->settings_access_ = ACCESS_PIN;
  else if (access == "hidden") this->settings_access_ = ACCESS_HIDDEN;
  else {
    ESP_LOGW(TAG, "Unknown settings access '%s'", access.c_str());
    return;
  }
  this->link_settings_page_();
  this->save_panel_settings_();
  this->report_panel_settings_();
  auto current = this->page_mgr_.current_page();
  if (current != nullptr && !current->is_type(page_type::screensaver)) this->render_current_page_();
}

void NSPanelLovelace::set_lock_display(std::string display) {
  if (display == "keypad") this->lock_shows_screensaver_ = false;
  else if (display == "screensaver") this->lock_shows_screensaver_ = true;
  else {
    ESP_LOGW(TAG, "Unknown lock display '%s'", display.c_str());
    return;
  }
  this->update_lock_card_();
  this->save_panel_settings_();
  this->report_panel_settings_();
  if (this->panel_locked_) this->render_current_page_();
}

bool NSPanelLovelace::process_locked_press_(const std::string &button_type) {
  // These end in render_current_page_(), which only allows the lock screen
  if (button_type == button_type::bExit || button_type == button_type::sleepReached) return false;
  ESP_LOGD(TAG, "Ignoring press while locked");
  return true;
}

void NSPanelLovelace::process_panel_row_press_(const std::string &entity_id,
    const std::string &button_type, const std::string &value) {
  if (entity_id == "nspanel.lock") {
    if (button_type != button_type::button) return;
    this->render_popup_notify_page_("nspanel.confirm_lock", get_translation("panel_lock_screen"),
        get_translation("panel_confirm_lock"), 0U,
        get_translation(translation_item::cancel), get_translation(translation_item::lock));
  } else if (entity_id == "nspanel.reboot") {
    if (button_type != button_type::button) return;
    this->render_popup_notify_page_("nspanel.confirm_reboot", get_translation("panel_reboot"),
        get_translation("panel_confirm_reboot"), 0U,
        get_translation(translation_item::cancel), get_translation("panel_reboot"));
  } else if (entity_id == "nspanel.confirm_reboot" || entity_id == "nspanel.confirm_lock") {
    if (button_type != NOTIFY_ACTION) return;
    if (value != "yes") {
      this->render_current_page_();
    } else if (entity_id == "nspanel.confirm_lock") {
      this->lock_panel();
    } else {
      ESP_LOGI(TAG, "Reboot requested from the panel");
      this->set_timeout(REBOOT_DELAY_MS, []() { App.safe_reboot(); });
    }
  } else if (entity_id == "nspanel.screen_off") {
    if (button_type != button_type::button) return;
    this->turn_screen_off_();
  } else if (entity_id == "nspanel.brightness") {
    if (button_type != button_type::numberSet) return;
    auto brightness = parse_number<uint8_t>(value);
    if (!brightness.has_value()) return;
    // passing both keeps the inactive brightness below the active one
    this->set_display_dim(this->display_inactive_dim_, std::max(*brightness, BRIGHTNESS_MIN));
    this->on_brightness_changed_();
  } else if (entity_id == "nspanel.screensaver_brightness") {
    if (button_type != button_type::numberSet) return;
    auto brightness = parse_number<uint8_t>(value);
    if (!brightness.has_value()) return;
    // The screen only reports a slider once it is let go, so show the result then
    this->previewing_inactive_dim_ = true;
    this->set_display_dim(*brightness, this->display_active_dim_);
    this->previewing_inactive_dim_ = false;
    this->on_brightness_changed_();
    this->set_timeout(BRIGHTNESS_PREVIEW_TIMEOUT, BRIGHTNESS_PREVIEW_MS, [this]() { this->set_display_dim(); });
  } else if (entity_id == "nspanel.button_lock") {
    if (button_type == button_type::onOff && !value.empty()) {
      this->set_button_lock(value == "1");
    } else if (button_type == button_type::button) {
      this->set_button_lock(!this->buttons_locked_);
    }
  }
}

void NSPanelLovelace::show_lock_message_(const char *translation_key) {
  this->lock_card_->set_title(get_translation(translation_key));
  this->render_current_page_();
  this->set_timeout(LOCK_TITLE_TIMEOUT, LOCK_MESSAGE_MS, [this]() {
    this->lock_card_->set_title(this->lock_title_());
    if (this->page_mgr_.current_page() == this->lock_card_) this->render_current_page_();
  });
}

void NSPanelLovelace::try_unlock_(const std::string &pin) {
  if (this->lock_failed_attempts_ >= LOCK_MAX_ATTEMPTS) {
    if (millis() - this->lock_failed_at_ < LOCK_COOLDOWN_MS) {
      this->show_lock_message_("panel_wait");
      return;
    }
    this->lock_failed_attempts_ = 0;
  }
  if (!is_valid_pin(pin) || hash_pin(pin) != this->lock_pin_hash_) {
    this->lock_failed_attempts_++;
    this->lock_failed_at_ = millis();
    ESP_LOGW(TAG, "Wrong PIN, attempt %u", this->lock_failed_attempts_);
    this->show_lock_message_("panel_wrong_pin");
    return;
  }
  if (this->panel_locked_) {
    this->unlock_panel();
    return;
  }
  // the keypad was asked for by opening the settings page
  this->lock_failed_attempts_ = 0;
  this->cancel_timeout(LOCK_TITLE_TIMEOUT);
  this->force_current_page_update_ = true;
  this->render_page_(this->settings_card_->get_uuid());
}

} // namespace nspanel_lovelace
} // namespace esphome

#endif // USE_NSPANEL_PANEL_SETTINGS
