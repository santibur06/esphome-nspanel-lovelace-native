#pragma once

#include "esphome/core/defines.h"
#ifdef USE_NSPANEL_RUNTIME_LAYOUT
#include "esphome/components/json/json_util.h"
#endif

#include "defines.h"

#include <functional>
#include <memory>
#include <map>
#include <queue>
#include <stdint.h>
#include <utility>
#include <vector>

#include "esphome/core/defines.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/core/string_ref.h"
#include "esphome/core/version.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/api/custom_api_device.h"

#include <driver/gpio.h>
#include "esphome/components/uart/uart_component_esp_idf.h"
#ifdef USE_NSPANEL_TFT_UPLOAD
// #ifndef USE_ARDUINO
#include <esp_http_client.h>
// #else
// #include <HTTPClient.h>
// #endif
#endif

#ifdef USE_TIME
#include "esphome/core/time.h"
#include "esphome/components/time/real_time_clock.h"
#ifdef USE_NSPANEL_TEMPERATURE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#endif

#include "config.h"
#include "entity.h"
#include "types.h"
#include "helpers.h"
#include "page_base.h"
#include "page_manager.h"
#include "card_base.h"
#include "pages.h"

#ifdef USE_NSPANEL_SCREEN_LIGHT
namespace esphome {
namespace light {
class LightState;
}
}
#endif

namespace esphome {
namespace nspanel_lovelace {

PACK(struct NSPanelRestoreState {
  uint8_t display_active_dim_ = 100;
  uint8_t display_inactive_dim_ = 50;
});

class AlarmCard;

class NSPanelLovelace : public Component, public uart::UARTDevice, public api::CustomAPIDevice {
public:
  NSPanelLovelace();
  void setup() override;
  void loop() override;

  std::shared_ptr<Entity> create_entity(const std::string &entity_id);

  template <class... TArgs>
  Screensaver* create_screensaver(TArgs&&... args) {
    if (this->screensaver_ == nullptr) {
      this->screensaver_ = this->page_mgr_
        .find_bookmarked_page<Screensaver>((uint8_t)render_page_option::screensaver_page);
      if (this->screensaver_ != nullptr)
        return this->screensaver_;
    }
    this->screensaver_ = this->page_mgr_
      .insert_page<Screensaver>(0, std::forward<TArgs>(args)...);
    // Ensure the page is skipped when cycling the pages
    this->screensaver_->set_hidden(true);
    this->screensaver_->set_on_item_added_callback(
      std::bind(&NSPanelLovelace::on_page_item_added_callback,
        this, std::placeholders::_1));
    return this->screensaver_;
  }

  template <typename TPage, typename... TArgs>
  TPage* create_page(TArgs&&... args) {
    static_assert(
      std::is_base_of<Page, TPage>::value,
      "TPage must derive from esphome::nspanel_lovelace::Page");
    TPage* page = this->page_mgr_.create_page<TPage>(std::forward<TArgs>(args)...);
    static_cast<Page*>(page)->set_on_item_added_callback(
      std::bind(&NSPanelLovelace::on_page_item_added_callback,
        this, std::placeholders::_1));
    return page;
  }

#ifdef USE_TIME
  void set_time_id(time::RealTimeClock *time_id) { this->time_id_ = time_id; }
  void set_date_format(const std::string &date_format) {
    if (date_format.empty()) return;
    this->date_format_ = date_format;
  }
  void set_time_format(const std::string &time_format) {
    if (time_format.empty()) return;
    this->time_format_ = time_format;
  }

  void update_date(const char *date_format = "") { this->update_datetime(datetime_mode::date, date_format); }
  void update_time(const char *time_format = "") { this->update_datetime(datetime_mode::time, "", time_format); }
  // for time formatting see: https://esphome.io/components/time/#strftime
  void update_datetime(const datetime_mode mode = datetime_mode::both, const char *date_format = "", const char *time_format = "");
#endif
  
  void on_page_item_added_callback(const std::shared_ptr<PageItem> &item);
  void set_language(const std::string &language) { this->language_ = language; }
  // Note: This can only be called after the requested page has been created.
  //       Returns true if sucessful, otherwise false.
  bool set_default_page(const std::string &uuid) {
    auto index = this->page_mgr_.find_page_index(uuid);
    if (index == SIZE_MAX) return false;
    return this->page_mgr_.bookmark_page(
      (uint8_t)render_page_option::default_page, index, true);
  }
  void set_display_timeout(uint16_t timeout);
  void set_display_active_dim(uint8_t active);
  void set_display_inactive_dim(uint8_t inactive);
  // Note: this can be used without parameters to update the display without changing the levels
  void set_display_dim(uint8_t inactive = UINT8_MAX, uint8_t active = UINT8_MAX);
#ifdef USE_NSPANEL_WEATHER_SERVICE
  void set_weather_forecast_data(std::string forecast_json);
#endif
  void set_weather_entity_id(const std::string &weather_entity_id) { this->weather_entity_id_ = weather_entity_id; }
#ifdef USE_NSPANEL_RUNTIME_LAYOUT
  void set_model(const std::string &model) { this->model_ = model; }
  // Validates and stores a layout (json), then reboots to apply it. An empty value removes the stored layout.
  void set_layout(std::string layout_json);
  void report_layout_status();
#ifdef USE_NSPANEL_RELAYS
  // A relay's light, and optionally its switch and physical button, which are reported to Home Assistant
  // so that it can show the ones wanted. The layout decides what each button does.
  void add_relay(light::LightState *light, EntityBase *relay_switch, EntityBase *button);
  // Does what the layout says for this physical button. Without a rule it toggles the button's own relay.
  void press_button(uint8_t index);
#endif
#endif

#ifdef USE_NSPANEL_PANEL_SETTINGS
  void lock_panel();
  void unlock_panel();
  // Digits only, 4 to 8 of them. An empty value removes the PIN and unlocks the panel.
  void set_lock_pin(std::string pin);
  // The hash of a PIN (8 hex characters), all zeros removes it
  void set_lock_pin_hash(std::string hash);
  void set_button_lock(bool locked);
#ifdef USE_NSPANEL_TEMPERATURE_SENSOR
  void set_temperature_sensor(sensor::Sensor *temperature_sensor) { this->temperature_sensor_ = temperature_sensor; }
#endif
  void set_settings_max_rows(uint8_t rows) { this->settings_max_rows_ = rows; }
#ifdef USE_NSPANEL_SCREEN_LIGHT
  void set_screen_light_state(light::LightState *state) { this->screen_light_ = state; }
  void set_screen_from_light(bool on, float brightness);
  void set_screensaver_light_state(light::LightState *state) { this->screensaver_light_ = state; }
  void set_screensaver_from_light(bool on, float brightness);
#endif
  // 'open', 'pin' (ignored while no PIN is set) or 'hidden'
  void set_settings_access(std::string access);
  // While locked: 'keypad' always shows the keypad, 'screensaver' shows it only when tapped
  void set_lock_display(std::string display);
  bool buttons_locked() const { return this->buttons_locked_; }
#endif

  bool get_double_tap_to_unlock() const { return this->double_tap_to_unlock_; }
  void set_double_tap_to_unlock(bool value) { this->double_tap_to_unlock_ = value; }
  void set_global_vertical_ui(bool val) { this->global_vertical_ui_ = val; }
  
  void render_screensaver_page() { this->render_page_(render_page_option::screensaver_page); }
  void render_next_page() { this->render_page_(render_page_option::next); }
  void render_previous_page() { this->render_page_(render_page_option::prev); }
  void render_default_page() { this->render_page_(render_page_option::default_page); }

  void notify_on_screensaver(const std::string &heading,
      const std::string &message, uint32_t timeout_ms = 0);

  void send_display_command(const std::string &command);
  /**
   * Softreset the Nextion
   */
  void soft_reset_display() {
    // this->send_nextion_command_("rest"); // only for stock FW
#ifndef USE_ARDUINO
    gpio_set_level(GPIO_NUM_4, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(GPIO_NUM_4, 0);
#else
    digitalWrite(4, 1);
    delay(1000);
    digitalWrite(4, 0);
#endif
  }

  float get_setup_priority() const override { return setup_priority::DATA; }

  void dump_config() override;

  void add_incoming_msg_callback(std::function<void(std::string)> callback) { this->incoming_msg_callback_.add(std::move(callback)); }

#ifdef TEST_DEVICE_MODE
  // Only used to simulate TFT commands on test devices
  void process_command(const std::string &message);
#endif

#ifdef USE_NSPANEL_TFT_UPLOAD
  /**
   * Upload the tft file and softreset the Nextion.
   * If the upload fails for any reason, a power cycle of the display and re-upload will be needed
   */
  bool upload_tft(const std::string &url);
#endif

protected:
  bool restore_state_();
  bool save_state_();
  ESPPreferenceObject pref_;

  void init_display_(int baud_rate);
#ifdef USE_NSPANEL_TFT_UPLOAD
  uint16_t recv_ret_string_(std::string &response, uint32_t timeout, bool recv_flag);
// #ifdef USE_ARDUINO
//   void set_reparse_mode_(bool active);
// #endif
#endif
  void send_nextion_command_(const std::string &command);

  void subscribe_homeassistant_state_attr(
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026,1,0)
    void (NSPanelLovelace::*callback)(const std::string &, const std::string &, esphome::StringRef),
#else
    void (NSPanelLovelace::*callback)(std::string, std::string, std::string),
#endif
    const std::string &entity_id, const std::string &attr_name
  ) {
    auto f = std::bind(callback, this, entity_id, attr_name, std::placeholders::_1);
    api::global_api_server->
      subscribe_home_assistant_state(entity_id, optional<std::string>(attr_name), std::move(f));
  }

  bool process_data_();
  const std::string &try_replace_uuid_with_entity_id_(const std::string &uuid_or_entity_id);
  void process_command_(const std::string &message);
  void send_buffered_command_();
  void process_display_command_queue_();
  void process_button_press_(std::string &entity_id,
    const std::string &button_type,
    const std::string &value = "", bool called_from_timeout = false);
  StatefulPageItem* get_page_item_(const std::string &uuid);
  Entity* get_entity_(const std::string &entity_id);

  void render_page_(const std::string &uuid);
  void render_page_(render_page_option d);
  void render_current_page_();
  void render_item_update_();
  void render_item_update_(Page *page);
  void render_popup_notify_page_(const std::string &internal_id,
    const std::string &heading, const std::string &message, uint16_t timeout = 0U,
    const std::string &btn1_text = "", const std::string &btn2_text = "");
  void render_popup_page_(const std::string &internal_id);
  bool render_popup_page_update_(const std::string &internal_id);
  bool render_popup_page_update_(StatefulPageItem *entity);
  void render_light_detail_update_(StatefulPageItem *entity);
  void render_timer_detail_update_(StatefulPageItem *entity);
  void render_cover_detail_update_(StatefulPageItem *item);
  void render_climate_detail_update_(StatefulPageItem *item);
  void render_climate_detail_update_(Entity *entity, const std::string &uuid = "");
  void render_input_select_detail_update_(StatefulPageItem *item);
  void render_fan_detail_update_(StatefulPageItem *item);

#ifdef USE_TIME
  void setup_time_();
  // Check and update clock if required
  void check_time_();
  optional<time::RealTimeClock *> time_id_{};
  std::string date_format_ = "%A, %d. %B %Y";
  std::string time_format_ = "%H:%M";
  uint8_t now_minute_, now_hour_;
  bool time_configured_;
#endif
  
  bool double_tap_to_unlock_ = false;

  uint8_t display_active_dim_ = 100;
  uint8_t display_inactive_dim_ = 50;
  
  void call_ha_service_(
    const std::string& service, const std::string& entity_id);
  void call_ha_service_(
    const char *entity_type, const std::string &action, const std::string& entity_id);
  void call_ha_service_(
    const char *entity_type, const std::string &action,
    const std::map<std::string, std::string> &data,
    const std::map<std::string, std::string> &data_template = {});
  void call_ha_service_(
    const std::string& service,
    const std::map<std::string, std::string> &data,
    const std::map<std::string, std::string> &data_template = {});
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026,1,0)
  void on_entity_state_update_(const std::string &entity_id, esphome::StringRef state);
  void on_entity_attribute_update_(
    const std::string &entity_id, const std::string &attr_name, esphome::StringRef attr_value);

  void on_weather_state_update_(const std::string &entity_id, esphome::StringRef state);
  void on_weather_temperature_update_(const std::string &entity_id, esphome::StringRef temperature);
  void on_weather_temperature_unit_update_(const std::string &entity_id, esphome::StringRef temperature_unit);
  void on_weather_forecast_update_(const std::string &entity_id, esphome::StringRef forecast_json);
#else
  void on_entity_state_update_(std::string entity_id, std::string state);
  void on_entity_attribute_update_(
    std::string entity_id, std::string attr_name, std::string attr_value);

  void on_weather_state_update_(std::string entity_id, std::string state);
  void on_weather_temperature_update_(std::string entity_id, std::string temperature);
  void on_weather_temperature_unit_update_(std::string entity_id, std::string temperature_unit);
  void on_weather_forecast_update_(std::string entity_id, std::string forecast_json);
#endif
  void send_weather_update_command_();
  std::string weather_entity_id_;
#ifdef USE_NSPANEL_PANEL_SETTINGS
  void setup_panel_settings_();
  void apply_lock_pin_hash_(uint32_t hash);
  void save_panel_settings_();
  void update_panel_entities_();
  void report_panel_settings_();
  void link_settings_page_();
  void setup_diagnostics_(uint16_t sleep_timeout);
  std::string diagnostics_value_(const std::string &entity_id) const;
  void update_lock_row_();
  void update_lock_card_();
  void wake_screen_(bool sync_light = true);
  void turn_screen_off_();
  void on_brightness_changed_();
  void sync_screen_light_();
  bool process_settings_nav_(const std::string &uuid);
  bool process_locked_press_(const std::string &button_type);
  void process_panel_row_press_(const std::string &entity_id,
    const std::string &button_type, const std::string &value);
  void try_unlock_(const std::string &pin);
  void show_lock_message_(const char *translation_key);
  const char *lock_title_() const;

  AlarmCard *lock_card_{nullptr};
  EntitiesCard *diagnostics_card_{nullptr};
#ifdef USE_NSPANEL_TEMPERATURE_SENSOR
  sensor::Sensor *temperature_sensor_{nullptr};
#endif
  Card *settings_card_{nullptr};
  std::shared_ptr<PageItem> lock_row_;
  uint8_t settings_max_rows_{6};
  bool has_screensaver_{false};
  bool screen_off_{false};
#ifdef USE_NSPANEL_SCREEN_LIGHT
  light::LightState *screen_light_{nullptr};
  light::LightState *screensaver_light_{nullptr};
  bool screen_light_ready_{false};
#endif
  bool panel_locked_{false};
  bool buttons_locked_{false};
  // Sends the screensaver brightness as the active one, so it shows on any page for a moment
  bool previewing_inactive_dim_{false};
  uint8_t settings_access_{0};
  bool lock_shows_screensaver_{false};
  uint16_t lock_sleep_timeout_{10};
  uint32_t lock_pin_hash_{0};
  uint8_t lock_failed_attempts_{0};
  uint32_t lock_failed_at_{0};
#else
  static constexpr bool panel_locked_ = false;
  static constexpr bool screen_off_ = false;
  static constexpr bool previewing_inactive_dim_ = false;
#endif
  // Set by a brightness rule in the layout, which then decides the dimming
  bool brightness_managed_{false};
#ifdef USE_NSPANEL_RUNTIME_LAYOUT
  struct BrightnessStep {
    uint16_t minute;
    uint8_t active;
    uint8_t inactive;
  };
  std::vector<BrightnessStep> brightness_steps_;
  int8_t brightness_step_applied_{-1};
  float brightness_entity_from_{0.0f};
  float brightness_entity_to_{100.0f};
  int16_t brightness_entity_inactive_low_{-1};
  int16_t brightness_entity_inactive_high_{-1};
  void build_brightness_(JsonObjectConst config);
#ifdef USE_NSPANEL_RELAYS
  struct Relay {
    light::LightState *light;
    EntityBase *relay_switch;
    EntityBase *button;
    // What the physical button does: a relay's index, or one of the values below
    int8_t button_target;
    std::string button_entity_id;
  };
  static constexpr int8_t BUTTON_TARGET_ENTITY = -1;
  static constexpr int8_t BUTTON_TARGET_NONE = -2;
  std::vector<Relay> relays_;
  bool validate_buttons_(JsonObjectConst layout, std::string &error) const;
  void build_buttons_(JsonObjectConst layout);
#endif
#ifdef USE_TIME
  void loop_brightness_steps_(const ESPTime &now);
#endif
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026,1,0)
  void on_brightness_entity_(const std::string &entity_id, esphome::StringRef state);
#else
  void on_brightness_entity_(std::string entity_id, std::string state);
#endif
  void load_stored_layout_();
  bool validate_layout_(JsonObjectConst layout, std::string &error);
  void build_layout_(JsonObjectConst layout);
  void loop_layout_status_();

  std::string model_{"eu"};
  // 'config' when using the pages from the device configuration, otherwise 'stored'
  std::string layout_source_{"config"};
  std::string layout_error_;
  uint32_t layout_hash_{0};
  bool layout_status_connected_{false};
#endif
  std::string language_;

  std::queue<std::string> command_queue_;
  unsigned long command_last_sent_ = 0;

  bool button_press_timeout_set_ = false;
  std::string button_press_uuid_;
  std::string button_press_type_;
  std::string button_press_value_;

  PageManager page_mgr_;
  std::string popup_page_current_uuid_;
  bool force_current_page_update_ = false;
  bool global_vertical_ui_{false};
  std::vector<std::shared_ptr<Entity>> entities_;
  std::vector<std::shared_ptr<StatefulPageItem>> stateful_page_items_;
  Screensaver* screensaver_ = nullptr;
  StatefulPageItem* cached_page_item_ = nullptr;
  Entity* cached_entity_ = nullptr;

  CallbackManager<void(std::string)> incoming_msg_callback_;

  std::vector<uint8_t> buffer_;
  std::string command_buffer_;

#ifdef USE_NSPANEL_TFT_UPLOAD
  uint32_t default_baud_rate_ = 0;
  uint32_t update_baud_rate_ = 921600;
  bool is_updating_ = false;
  bool reparse_mode_ = false;
  uint32_t content_length_ = 0;
  size_t tft_size_ = 0;
  bool upload_first_chunk_sent_ = false;
// #ifndef USE_ARDUINO
  int upload_by_chunks_(esp_http_client_handle_t http_client, uint32_t &range_start);
// #else
//   uint8_t *transfer_buffer_ = nullptr;
//   size_t transfer_buffer_size_;
//   int upload_by_chunks_(HTTPClient *http, const std::string &url, uint32_t &range_start);
// #endif
  bool upload_end_(bool successful);
#endif // USE_NSPANEL_TFT_UPLOAD
};

}  // namespace nspanel_lovelace
}  // namespace esphome
