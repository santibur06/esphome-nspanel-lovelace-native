#include "nspanel_lovelace.h"

#ifdef USE_NSPANEL_RUNTIME_LAYOUT

#include "cards.h"
#include "card_items.h"
#include "page_items.h"
#include "pages.h"
#include "types.h"
#include "esphome/components/json/json_util.h"
#ifdef USE_NSPANEL_RELAYS
#include "esphome/components/light/light_state.h"
#endif
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <nvs.h>
#include <set>

namespace esphome {
namespace nspanel_lovelace {

static const char *const TAG = "nspanel_lovelace.layout";

static const char *const LAYOUT_NVS_NAMESPACE = "nspanel";
static const char *const LAYOUT_NVS_KEY = "layout";
static const size_t LAYOUT_MAX_SIZE = 16384;
static const size_t LAYOUT_MAX_CARDS = 32;
static const uint32_t LAYOUT_REBOOT_DELAY_MS = 500;
static const uint32_t LAYOUT_STATUS_DELAY_MS = 3000;
static const size_t BRIGHTNESS_MAX_STEPS = 8;
static const int BRIGHTNESS_MIN = 5;

/*
 * =============== helpers ===============
 */

static bool load_layout_blob(std::string &out) {
  nvs_handle_t handle;
  if (nvs_open(LAYOUT_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
  size_t size = 0;
  bool ok = nvs_get_blob(handle, LAYOUT_NVS_KEY, nullptr, &size) == ESP_OK &&
      size > 0 && size <= LAYOUT_MAX_SIZE;
  if (ok) {
    out.resize(size);
    ok = nvs_get_blob(handle, LAYOUT_NVS_KEY, &out[0], &size) == ESP_OK;
  }
  nvs_close(handle);
  return ok;
}

static bool save_layout_blob(const std::string &layout) {
  nvs_handle_t handle;
  if (nvs_open(LAYOUT_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
  esp_err_t err = layout.empty()
      ? nvs_erase_key(handle, LAYOUT_NVS_KEY)
      : nvs_set_blob(handle, LAYOUT_NVS_KEY, layout.data(), layout.size());
  if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err == ESP_OK;
}

// Icons are referenced by pointer for the lifetime of the page item, so the
// bytes are kept here. A layout change reboots, so this never needs to shrink.
static const icon_char_t *get_icon_from_hex(const std::string &hex) {
  static std::map<std::string, std::string> icons;
  auto found = icons.find(hex);
  if (found == icons.end()) {
    uint32_t code = strtoul(hex.c_str(), nullptr, 16);
    std::string utf8;
    utf8.push_back(static_cast<char>(0xE0 | ((code >> 12) & 0x0F)));
    utf8.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    utf8.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    found = icons.emplace(hex, std::move(utf8)).first;
  }
  return reinterpret_cast<const icon_char_t *>(found->second.c_str());
}

static bool is_valid_icon(const char *hex) {
  if (strlen(hex) != 4) return false;
  for (size_t i = 0; i < 4; ++i) {
    if (!isxdigit(static_cast<unsigned char>(hex[i]))) return false;
  }
  return true;
}

static bool is_valid_entity_id(const char *id) {
  if (strcmp(id, "delete") == 0) return true;
  // these rows belong to the built in settings page
  if (strncmp(id, "nspanel.", 8) == 0) return false;
  // free text is allowed after 'iText.', see valid_entity_id() in __init__.py
  if (strncmp(id, "iText.", 6) == 0) return strchr(id, SEPARATOR) == nullptr;
  const char *dot = strchr(id, '.');
  if (dot == nullptr || dot == id || dot[1] == '\0') return false;
  for (const char *c = id; *c != '\0'; ++c) {
    if (c != dot && !isalnum(static_cast<unsigned char>(*c)) && *c != '_') return false;
  }
  return true;
}

static bool has_separator(const char *text) {
  return strchr(text, SEPARATOR) != nullptr;
}

static bool get_alarm_action(const char *name, alarm_arm_action &action) {
  if (strcmp(name, "arm_home") == 0) action = alarm_arm_action::arm_home;
  else if (strcmp(name, "arm_away") == 0) action = alarm_arm_action::arm_away;
  else if (strcmp(name, "arm_night") == 0) action = alarm_arm_action::arm_night;
  else if (strcmp(name, "arm_vacation") == 0) action = alarm_arm_action::arm_vacation;
  else if (strcmp(name, "arm_custom_bypass") == 0) action = alarm_arm_action::arm_custom_bypass;
  else return false;
  return true;
}

// Same limits as get_card_entities_length_limits() in __init__.py
static size_t get_max_card_entities(const std::string &type, const std::string &model) {
  if (type == "cardEntities") return (model == "eu" || model == "us-l") ? 4 : 6;
  if (type == "cardGrid") return 6;
  if (type == "cardGrid2") return 8;
  if (type == "cardQR") return 2;
  if (type == "cardMedia") return 8;
  return 0;
}

static const char *get_card_entity_key(const std::string &type) {
  if (type == "cardThermo") return "thermo_entity_id";
  if (type == "cardAlarm") return "alarm_entity_id";
  if (type == "cardMedia") return "media_entity_id";
  return nullptr;
}

template<typename TObject>
static bool validate_icon(const TObject &obj, std::string &error, const char *where) {
  const char *icon = obj["icon"] | "";
  if (*icon == '\0' || is_valid_icon(icon)) return true;
  error = std::string(where) + ": icon must be 4 hex characters";
  return false;
}

static bool validate_screensaver_entity(JsonObjectConst entity, std::string &error, const char *where) {
  const char *entity_id = entity["entity_id"] | "";
  if (!is_valid_entity_id(entity_id) || strcmp(entity_id, "delete") == 0) {
    error = std::string(where) + ": invalid entity_id";
    return false;
  }
  if (has_separator(entity["name"] | "")) {
    error = std::string(where) + ": name contains an invalid character";
    return false;
  }
  return validate_icon(entity, error, where);
}

/*
 * =============== validation ===============
 */

static bool parse_step_time(const char *text, uint16_t &minute) {
  unsigned hour = 0, min = 0;
  if (sscanf(text, "%2u:%2u", &hour, &min) != 2 || hour > 23 || min > 59) return false;
  minute = hour * 60 + min;
  return true;
}

static bool validate_brightness(JsonObjectConst config, std::string &error) {
  auto in_range = [](JsonVariantConst value, int min, int max) {
    return value.is<int>() && value.as<int>() >= min && value.as<int>() <= max;
  };
  std::string mode = config["mode"] | "";
  if (mode == "schedule") {
    JsonArrayConst steps = config["schedule"];
    if (steps.size() == 0 || steps.size() > BRIGHTNESS_MAX_STEPS) {
      error = "brightness: the schedule needs 1 to " + std::to_string(BRIGHTNESS_MAX_STEPS) + " times";
      return false;
    }
    for (JsonObjectConst step : steps) {
      uint16_t minute;
      if (!parse_step_time(step["time"] | "", minute)) {
        error = "brightness: invalid time";
        return false;
      }
      if (!in_range(step["active"], BRIGHTNESS_MIN, 100) || !in_range(step["inactive"], 0, 100)) {
        error = "brightness: values must be 5 to 100, and 0 to 100 on standby";
        return false;
      }
    }
    return true;
  }
  if (mode == "entity") {
    if (!is_valid_entity_id(config["entity_id"] | "")) {
      error = "brightness: invalid entity_id";
      return false;
    }
    if (!config["from"].is<float>() || !config["to"].is<float>() ||
        config["from"].as<float>() == config["to"].as<float>()) {
      error = "brightness: the entity needs two different values for 'from' and 'to'";
      return false;
    }
    bool has_low = !config["inactive_low"].isNull();
    bool has_high = !config["inactive_high"].isNull();
    if (has_low != has_high) {
      error = "brightness: set both standby values or neither";
      return false;
    }
    if (has_low && (!in_range(config["inactive_low"], 0, 100) || !in_range(config["inactive_high"], 0, 100))) {
      error = "brightness: the standby values must be 0 to 100";
      return false;
    }
    return true;
  }
  error = "brightness: unknown mode";
  return false;
}

#ifdef USE_NSPANEL_RELAYS
// The first relay is on the left
static int side_index(const char *side) {
  if (strcmp(side, "left") == 0) return 0;
  if (strcmp(side, "right") == 0) return 1;
  return -1;
}

bool NSPanelLovelace::validate_buttons_(JsonObjectConst layout, std::string &error) const {
  auto valid_side = [this](const char *side) {
    int index = side_index(side);
    return index >= 0 && index < static_cast<int>(this->relays_.size());
  };
  for (JsonPairConst pair : layout["buttons"].as<JsonObjectConst>()) {
    if (!valid_side(pair.key().c_str())) {
      error = std::string("buttons: this panel has no '") + pair.key().c_str() + "' button";
      return false;
    }
    JsonObjectConst button = pair.value();
    std::string action = button["action"] | "";
    if (action == "entity") {
      if (!is_valid_entity_id(button["entity_id"] | "")) {
        error = "buttons: invalid entity_id";
        return false;
      }
    } else if (!action.empty() && action != "none" && !valid_side(action.c_str())) {
      error = "buttons: unknown action '" + action + "'";
      return false;
    }
  }
  return true;
}

void NSPanelLovelace::build_buttons_(JsonObjectConst layout) {
  for (JsonPairConst pair : layout["buttons"].as<JsonObjectConst>()) {
    auto &relay = this->relays_[side_index(pair.key().c_str())];
    JsonObjectConst button = pair.value();
    std::string action = button["action"] | "";
    if (action == "entity") {
      relay.button_target = BUTTON_TARGET_ENTITY;
      relay.button_entity_id = button["entity_id"] | "";
      this->create_entity(relay.button_entity_id);
    } else if (action == "none") {
      relay.button_target = BUTTON_TARGET_NONE;
    } else if (!action.empty()) {
      relay.button_target = side_index(action.c_str());
    }
  }
}

void NSPanelLovelace::add_relay(light::LightState *light, EntityBase *relay_switch, EntityBase *button) {
  this->relays_.push_back({light, relay_switch, button, static_cast<int8_t>(this->relays_.size()), ""});
}

void NSPanelLovelace::press_button(uint8_t index) {
  if (index >= this->relays_.size()) return;
#ifdef USE_NSPANEL_PANEL_SETTINGS
  if (this->buttons_locked_) return;
#endif
  auto &relay = this->relays_[index];
  if (relay.button_target >= 0) {
    this->relays_[relay.button_target].light->toggle().perform();
  } else if (relay.button_target == BUTTON_TARGET_ENTITY) {
    const std::string &entity_id = relay.button_entity_id;
    std::string domain = entity_id.substr(0, entity_id.find('.'));
    if (domain == "button" || domain == "input_button") {
      this->call_ha_service_(domain.c_str(), "press", entity_id);
    } else if (domain == "scene") {
      this->call_ha_service_(domain.c_str(), ha_action_type::turn_on, entity_id);
    } else {
      this->call_ha_service_("homeassistant", ha_action_type::toggle, entity_id);
    }
  }
}
#endif

bool NSPanelLovelace::validate_layout_(JsonObjectConst layout, std::string &error) {
#ifdef USE_NSPANEL_RELAYS
  if (!this->validate_buttons_(layout, error)) return false;
#else
  if (!layout["buttons"].isNull()) {
    error = "buttons: this firmware has no relays configured";
    return false;
  }
#endif
  JsonObjectConst brightness = layout["brightness"];
  if (!brightness.isNull() && !validate_brightness(brightness, error)) return false;
#ifdef USE_TIME
  if (!brightness.isNull() && (brightness["mode"] | "") == std::string("schedule") &&
      !this->time_id_.has_value()) {
    error = "brightness: the schedule needs a time_id in the device configuration";
    return false;
  }
#endif

  JsonObjectConst screensaver = layout["screensaver"];
#ifdef USE_TIME
  if (!screensaver.isNull() && !this->time_id_.has_value()) {
    error = "screensaver needs a screensaver time_id in the device configuration";
    return false;
  }
#endif
  if (!screensaver.isNull()) {
    for (const char *key : {"main_entity", "extra_entity", "status_icon_left", "status_icon_right"}) {
      JsonObjectConst entity = screensaver[key];
      if (!entity.isNull() && !validate_screensaver_entity(entity, error, key)) return false;
    }
    for (const char *key : {"date_format", "time_format"}) {
      if (strlen(screensaver[key] | "") > 32) {
        error = std::string(key) + " must be at most 32 characters long";
        return false;
      }
    }
    const char *weather_id = screensaver["weather"]["entity_id"] | "";
    if (*weather_id != '\0' && !is_valid_entity_id(weather_id)) {
      error = "weather: invalid entity_id";
      return false;
    }
  }

  JsonArrayConst cards = layout["cards"];
  if (cards.isNull() || cards.size() == 0) {
    error = "at least one card is required";
    return false;
  }
  if (cards.size() > LAYOUT_MAX_CARDS) {
    error = "too many cards";
    return false;
  }

  std::set<std::string> card_ids;
  size_t visible_cards = 0;
  for (JsonObjectConst card : cards) {
    const char *id = card["id"] | "";
    if (*id != '\0' && !card_ids.insert(id).second) {
      error = std::string("duplicate card id: ") + id;
      return false;
    }
    if (!(card["hidden"] | false)) ++visible_cards;
  }
  if (visible_cards == 0) {
    error = "at least one card must be visible";
    return false;
  }
  if (screensaver.isNull() && (cards[0]["hidden"] | false)) {
    error = "the first card cannot be hidden if the screensaver is disabled";
    return false;
  }

  const char *default_card = layout["default_card"] | "";
  if (*default_card != '\0' && card_ids.count(default_card) == 0) {
    error = std::string("default_card not found: ") + default_card;
    return false;
  }

  size_t index = 0;
  for (JsonObjectConst card : cards) {
    std::string where = std::string("card ") + std::to_string(index++);
    std::string type = card["type"] | "";
    if (type != "cardEntities" && type != "cardGrid" && type != "cardGrid2" &&
        type != "cardQR" && type != "cardAlarm" && type != "cardThermo" && type != "cardMedia") {
      error = where + ": unknown type '" + type + "'";
      return false;
    }
    if (has_separator(card["title"] | "") || has_separator(card["qr_text"] | "")) {
      error = where + ": text contains an invalid character";
      return false;
    }
    int sleep_timeout = card["sleep_timeout"] | 10;
    if (sleep_timeout < 0 || sleep_timeout > 120) {
      error = where + ": sleep_timeout must be between 0 and 120";
      return false;
    }

    const char *entity_key = get_card_entity_key(type);
    if (entity_key != nullptr) {
      const char *entity_id = card[entity_key] | "";
      if (!is_valid_entity_id(entity_id) || strcmp(entity_id, "delete") == 0) {
        error = where + ": invalid " + entity_key;
        return false;
      }
    }

    if (type == "cardAlarm") {
      alarm_arm_action action;
      for (JsonVariantConst mode : card["supported_modes"].as<JsonArrayConst>()) {
        if (!get_alarm_action(mode | "", action)) {
          error = where + ": unknown alarm mode";
          return false;
        }
      }
    }

    JsonArrayConst entities = card["entities"];
    size_t max_entities = get_max_card_entities(type, this->model_);
    if (entities.size() > max_entities) {
      error = where + ": at most " + std::to_string(max_entities) + " entities allowed";
      return false;
    }
    bool needs_entities = type == "cardEntities" || type == "cardGrid" || type == "cardGrid2" || type == "cardQR";
    if (needs_entities && entities.size() == 0) {
      error = where + ": at least one entity is required";
      return false;
    }
    for (JsonObjectConst entity : entities) {
      const char *entity_id = entity["entity_id"] | "";
      if (!is_valid_entity_id(entity_id)) {
        error = where + ": invalid entity_id '" + entity_id + "'";
        return false;
      }
      if (strncmp(entity_id, "navigate.", 9) == 0 && card_ids.count(entity_id + 9) == 0) {
        error = where + ": no card has the id '" + (entity_id + 9) + "'";
        return false;
      }
      if (has_separator(entity["name"] | "")) {
        error = where + ": name contains an invalid character";
        return false;
      }
      if (!validate_icon(entity, error, where.c_str())) return false;
    }
  }
  return true;
}

/*
 * =============== page creation ===============
 */

template<typename TItem>
static void add_card_entities(Page *page, JsonArrayConst entities, page_type type,
    NSPanelLovelace *parent, size_t &uuid_index) {
  for (JsonObjectConst config : entities) {
    const char *entity_id = config["entity_id"] | "delete";
    if (strcmp(entity_id, "delete") == 0) {
      page->add_item(std::make_shared<DeleteItem>(type));
      continue;
    }
    auto uuid = std::string("r") + std::to_string(++uuid_index);
    auto entity = parent->create_entity(entity_id);
    std::shared_ptr<TItem> item;
    if (config["name"].is<const char *>()) {
      item = std::make_shared<TItem>(uuid, entity, std::string(config["name"].as<const char *>()));
    } else {
      item = std::make_shared<TItem>(uuid, entity);
    }
    const char *icon = config["icon"] | "";
    if (*icon != '\0') item->set_icon_value(get_icon_from_hex(icon));
    if (config["color"].is<int>()) item->set_icon_color(static_cast<uint16_t>(config["color"].as<int>()));
    if (config["assumed_state"] | false) item->set_assumed_state(true);
    page->add_item(item);
  }
}

static std::shared_ptr<ScreensaverEntityItem> create_screensaver_entity(
    JsonObjectConst config, NSPanelLovelace *parent, size_t &uuid_index) {
  auto item = std::make_shared<ScreensaverEntityItem>(
      std::string("r") + std::to_string(++uuid_index),
      parent->create_entity(config["entity_id"] | ""),
      std::string(config["name"] | ""));
  const char *icon = config["icon"] | "";
  if (*icon != '\0') item->set_icon_value(get_icon_from_hex(icon));
  if (config["color"].is<int>()) item->set_icon_color(static_cast<uint16_t>(config["color"].as<int>()));
  return item;
}

static std::shared_ptr<StatusIconItem> create_status_icon(
    JsonObjectConst config, NSPanelLovelace *parent, size_t &uuid_index) {
  auto item = std::make_shared<StatusIconItem>(
      std::string("r") + std::to_string(++uuid_index),
      parent->create_entity(config["entity_id"] | ""),
      icon_t::alert_circle_outline);
  const char *icon = config["icon"] | "";
  if (*icon != '\0') item->set_icon_value(get_icon_from_hex(icon));
  if (config["color"].is<int>()) item->set_icon_color(static_cast<uint16_t>(config["color"].as<int>()));
  if (config["alt_font"] | false) item->set_alt_font(true);
  return item;
}

template<typename TCard>
static TCard *set_card_nav(TCard *card, const std::string &left_uuid, const char *left_icon_hex,
    const std::string &right_uuid, size_t &uuid_index) {
  if (!left_uuid.empty()) {
    auto nav = std::unique_ptr<NavigationItem>(new NavigationItem(
        std::string("r") + std::to_string(++uuid_index), left_uuid, get_icon_from_hex(left_icon_hex)));
    card->set_nav_left(nav);
  }
  if (!right_uuid.empty()) {
    auto nav = std::unique_ptr<NavigationItem>(new NavigationItem(
        std::string("r") + std::to_string(++uuid_index), right_uuid, get_icon_from_hex("E733")));
    card->set_nav_right(nav);
  }
  return card;
}

void NSPanelLovelace::build_layout_(JsonObjectConst layout) {
  // Anything created from the device configuration is replaced entirely
  this->stateful_page_items_.clear();
  this->screensaver_ = nullptr;
  this->page_mgr_.clear();
  this->entities_.clear();
  this->weather_entity_id_.clear();

  JsonObjectConst brightness = layout["brightness"];
  if (!brightness.isNull()) this->build_brightness_(brightness);
#ifdef USE_NSPANEL_RELAYS
  this->build_buttons_(layout);
#endif

  size_t uuid_index = 0;
  auto new_uuid = [&uuid_index]() { return std::string("r") + std::to_string(++uuid_index); };

  if (layout["sleep_timeout"].is<int>()) {
    this->set_display_timeout(layout["sleep_timeout"].as<int>());
  }

  JsonObjectConst screensaver_config = layout["screensaver"];
  std::string home_uuid;
  if (screensaver_config.isNull()) {
    this->set_display_timeout(0);
  } else {
    home_uuid = new_uuid();
    auto screensaver = this->create_screensaver(home_uuid);

#ifdef USE_TIME
    this->set_date_format(screensaver_config["date_format"] | "");
    this->set_time_format(screensaver_config["time_format"] | "");
#endif
    this->set_double_tap_to_unlock(screensaver_config["double_tap_to_unlock"] | false);

    if (!screensaver_config["status_icon_left"].isNull()) {
      screensaver->set_icon_left(create_status_icon(screensaver_config["status_icon_left"], this, uuid_index));
    }
    if (!screensaver_config["status_icon_right"].isNull()) {
      screensaver->set_icon_right(create_status_icon(screensaver_config["status_icon_right"], this, uuid_index));
    }

    const char *weather_id = screensaver_config["weather"]["entity_id"] | "";
    if (*weather_id != '\0') {
      this->set_weather_entity_id(weather_id);
      // 1 main item (weather or HA entity) + 4 forecast items + optional extra HA entity
      std::vector<std::shared_ptr<PageItem>> items;
      if (!screensaver_config["main_entity"].isNull()) {
        items.push_back(create_screensaver_entity(screensaver_config["main_entity"], this, uuid_index));
      } else {
        items.push_back(std::make_shared<WeatherItem>(new_uuid()));
      }
      for (uint8_t i = 1; i < 5; ++i) {
        items.push_back(std::make_shared<WeatherItem>(new_uuid()));
      }
      if (!screensaver_config["extra_entity"].isNull()) {
        items.push_back(create_screensaver_entity(screensaver_config["extra_entity"], this, uuid_index));
      }
      screensaver->add_item_range(items);
    }
  }

  JsonArrayConst cards = layout["cards"];

  std::vector<std::string> card_uuids;
  std::vector<std::string> visible_card_uuids;
  for (JsonObjectConst card : cards) {
    const char *id = card["id"] | "";
    card_uuids.push_back(*id != '\0' ? std::string(id) : new_uuid());
    if (!(card["hidden"] | false)) visible_card_uuids.push_back(card_uuids.back());
  }

  std::map<std::string, std::string> parent_card_uuids;
  size_t index = 0;
  for (JsonObjectConst card : cards) {
    for (JsonObjectConst entity : card["entities"].as<JsonArrayConst>()) {
      const char *entity_id = entity["entity_id"] | "";
      if (strncmp(entity_id, "navigate.", 9) == 0) {
        parent_card_uuids.emplace(entity_id + 9, card_uuids[index]);
      }
    }
    ++index;
  }

  if (home_uuid.empty() && !visible_card_uuids.empty()) home_uuid = visible_card_uuids[0];

  size_t visible_index = 0;
  index = 0;
  for (JsonObjectConst config : cards) {
    const std::string &uuid = card_uuids[index];
    std::string type = config["type"] | "";
    std::string title = config["title"] | "";
    uint16_t sleep_timeout = config["sleep_timeout"] | 10;
    if (index == 0 && screensaver_config.isNull()) {
      // The default card can't time out when there is no screensaver to go to
      sleep_timeout = 0;
    }

    std::string nav_left, nav_right;
    const char *nav_left_icon = "E730"; // arrow-left-bold
    bool hidden = config["hidden"] | false;
    if (hidden) {
      auto parent = parent_card_uuids.find(config["id"] | "");
      if (parent != parent_card_uuids.end()) {
        nav_left = parent->second;
        nav_left_icon = "E736"; // arrow-up-bold
      } else {
        nav_left = home_uuid;
        nav_left_icon = "E2DB"; // home
      }
    } else {
      size_t count = visible_card_uuids.size();
      // a single visible card has nowhere to page to, so it gets no arrows
      if (count > 1) {
        nav_left = visible_card_uuids[(visible_index + count - 1) % count];
        nav_right = visible_card_uuids[(visible_index + 1) % count];
      }
      ++visible_index;
    }

    Page *page = nullptr;
    JsonArrayConst entities = config["entities"];
    if (type == "cardEntities") {
      auto card = this->create_page<EntitiesCard>(uuid, title, sleep_timeout);
      set_card_nav(card, nav_left, nav_left_icon, nav_right, uuid_index);
      add_card_entities<EntitiesCardEntityItem>(card, entities, page_type::cardEntities, this, uuid_index);
      page = card;
    } else if (type == "cardGrid" || type == "cardGrid2") {
      auto render_type = type == "cardGrid2" ? page_type::cardGrid2 : page_type::cardGrid;
      auto card = this->create_page<GridCard>(uuid, title, sleep_timeout);
      if (type == "cardGrid2") card->set_render_type(render_type);
      set_card_nav(card, nav_left, nav_left_icon, nav_right, uuid_index);
      add_card_entities<GridCardEntityItem>(card, entities, render_type, this, uuid_index);
      page = card;
    } else if (type == "cardQR") {
      auto card = this->create_page<QRCard>(uuid, title, sleep_timeout);
      set_card_nav(card, nav_left, nav_left_icon, nav_right, uuid_index);
      card->set_qr_text(config["qr_text"] | "");
      add_card_entities<EntitiesCardEntityItem>(card, entities, page_type::cardQR, this, uuid_index);
      page = card;
    } else if (type == "cardThermo") {
      auto card = this->create_page<ThermoCard>(
          uuid, this->create_entity(config["thermo_entity_id"] | ""), title, sleep_timeout);
      set_card_nav(card, nav_left, nav_left_icon, nav_right, uuid_index);
      for (JsonVariantConst mode : config["supported_modes"].as<JsonArrayConst>()) {
        card->add_supported_mode(mode | "");
      }
      page = card;
    } else if (type == "cardAlarm") {
      auto card = this->create_page<AlarmCard>(
          uuid, this->create_entity(config["alarm_entity_id"] | ""), title, sleep_timeout);
      set_card_nav(card, nav_left, nav_left_icon, nav_right, uuid_index);
      alarm_arm_action action;
      JsonArrayConst modes = config["supported_modes"];
      if (modes.size() == 0) {
        for (auto mode : {alarm_arm_action::arm_home, alarm_arm_action::arm_away,
                          alarm_arm_action::arm_night, alarm_arm_action::arm_vacation}) {
          card->add_arm_button(mode);
        }
      }
      for (JsonVariantConst mode : modes) {
        if (get_alarm_action(mode | "", action)) card->add_arm_button(action);
      }
      page = card;
    } else if (type == "cardMedia") {
      auto card = this->create_page<MediaCard>(
          uuid, this->create_entity(config["media_entity_id"] | ""), title, sleep_timeout);
      set_card_nav(card, nav_left, nav_left_icon, nav_right, uuid_index);
      add_card_entities<GridCardEntityItem>(card, entities, page_type::cardMedia, this, uuid_index);
      page = card;
    }
    if (page != nullptr && hidden) page->set_hidden(true);
    ++index;
  }

  const char *default_card = layout["default_card"] | "";
  if (*default_card != '\0') this->set_default_page(default_card);
}

/*
 * =============== storage and service ===============
 */

void NSPanelLovelace::build_brightness_(JsonObjectConst config) {
  this->brightness_managed_ = true;
  if (strcmp(config["mode"] | "", "entity") == 0) {
    this->brightness_entity_from_ = config["from"].as<float>();
    this->brightness_entity_to_ = config["to"].as<float>();
    this->brightness_entity_inactive_low_ = config["inactive_low"] | -1;
    this->brightness_entity_inactive_high_ = config["inactive_high"] | -1;
    this->subscribe_homeassistant_state(
        &NSPanelLovelace::on_brightness_entity_, std::string(config["entity_id"] | ""));
    return;
  }
  for (JsonObjectConst step : config["schedule"].as<JsonArrayConst>()) {
    uint16_t minute = 0;
    parse_step_time(step["time"] | "", minute);
    this->brightness_steps_.push_back({minute, static_cast<uint8_t>(step["active"].as<int>()), static_cast<uint8_t>(step["inactive"].as<int>())});
  }
  std::sort(this->brightness_steps_.begin(), this->brightness_steps_.end(),
      [](const BrightnessStep &a, const BrightnessStep &b) { return a.minute < b.minute; });
}

#ifdef USE_TIME
void NSPanelLovelace::loop_brightness_steps_(const ESPTime &now) {
  if (this->brightness_steps_.empty()) return;
  uint16_t minute = now.hour * 60 + now.minute;
  // Before the first time of the day, the last one from yesterday still applies
  int8_t index = this->brightness_steps_.size() - 1;
  for (size_t i = 0; i < this->brightness_steps_.size(); i++) {
    if (this->brightness_steps_[i].minute <= minute) index = i;
  }
  // Only on a change, so that the slider and the light keep working until the next time
  if (index == this->brightness_step_applied_) return;
  this->brightness_step_applied_ = index;
  auto &step = this->brightness_steps_[index];
  this->set_display_dim(step.inactive, step.active);
#ifdef USE_NSPANEL_PANEL_SETTINGS
  this->on_brightness_changed_();
#endif
}
#endif

void NSPanelLovelace::on_brightness_entity_(
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026,1,0)
    const std::string &entity_id, esphome::StringRef state
#else
    std::string entity_id, std::string state
#endif
) {
  auto value = parse_number<float>(std::string(state.c_str(), state.size()));
  if (!value.has_value()) return;
  float position = (*value - this->brightness_entity_from_) /
      (this->brightness_entity_to_ - this->brightness_entity_from_);
  position = std::max(0.0f, std::min(1.0f, position));
  uint8_t active = BRIGHTNESS_MIN + static_cast<uint8_t>(position * (100 - BRIGHTNESS_MIN) + 0.5f);
  uint8_t inactive = this->display_inactive_dim_;
  if (this->brightness_entity_inactive_low_ >= 0) {
    int range = this->brightness_entity_inactive_high_ - this->brightness_entity_inactive_low_;
    inactive = this->brightness_entity_inactive_low_ + static_cast<int>(position * range + 0.5f);
  }
  if (active == this->display_active_dim_ && inactive == this->display_inactive_dim_) return;
  this->set_display_dim(inactive, active);
#ifdef USE_NSPANEL_PANEL_SETTINGS
  this->on_brightness_changed_();
#endif
}

void NSPanelLovelace::load_stored_layout_() {
  std::string stored;
  if (!load_layout_blob(stored)) return;

  JsonDocument doc;
  auto parse_error = deserializeJson(doc, stored);
  std::string error;
  if (parse_error) {
    error = parse_error.c_str();
  } else if (this->validate_layout_(doc.as<JsonObjectConst>(), error)) {
    this->build_layout_(doc.as<JsonObjectConst>());
    this->layout_source_ = "stored";
    this->layout_hash_ = fnv1_hash(stored);
    ESP_LOGI(TAG, "Using stored layout %08" PRIx32 " (%zu bytes)", this->layout_hash_, stored.size());
    return;
  }
  // Keep the device usable with the pages from its configuration
  this->layout_error_ = error;
  ESP_LOGE(TAG, "Stored layout rejected, using the device configuration: %s", error.c_str());
}

void NSPanelLovelace::set_layout(std::string layout_json) {
  if (layout_json.empty()) {
    if (this->layout_source_ == "config") {
      this->report_layout_status();
      return;
    }
    ESP_LOGI(TAG, "Removing stored layout");
    if (!save_layout_blob(layout_json)) {
      this->layout_error_ = "could not write to flash";
      this->report_layout_status();
      return;
    }
    this->set_timeout(LAYOUT_REBOOT_DELAY_MS, []() { App.safe_reboot(); });
    return;
  }

  std::string error;
  if (layout_json.size() > LAYOUT_MAX_SIZE) {
    error = "layout is too large";
  } else {
    JsonDocument doc;
    auto parse_error = deserializeJson(doc, layout_json);
    if (parse_error) {
      error = parse_error.c_str();
    } else {
      this->validate_layout_(doc.as<JsonObjectConst>(), error);
    }
  }
  if (error.empty() && !save_layout_blob(layout_json)) {
    error = "could not write to flash";
  }

  this->layout_error_ = error;
  if (!error.empty()) {
    ESP_LOGE(TAG, "Layout rejected: %s", error.c_str());
    this->report_layout_status();
    return;
  }

  uint32_t hash = fnv1_hash(layout_json);
  if (this->layout_source_ == "stored" && hash == this->layout_hash_) {
    this->report_layout_status();
    return;
  }
  ESP_LOGI(TAG, "Layout %08" PRIx32 " saved, rebooting to apply it", hash);
  this->set_timeout(LAYOUT_REBOOT_DELAY_MS, []() { App.safe_reboot(); });
}

void NSPanelLovelace::report_layout_status() {
  char hash[9];
  snprintf(hash, sizeof(hash), "%08" PRIx32, this->layout_hash_);
  std::map<std::string, std::string> data = {
    {"device", App.get_name()},
    {"model", this->model_},
    {"source", this->layout_source_},
    {"hash", hash},
    {"error", this->layout_error_},
  };
#ifdef USE_NSPANEL_PANEL_SETTINGS
  data["locked"] = this->panel_locked_ ? "1" : "0";
  data["buttons_locked"] = this->buttons_locked_ ? "1" : "0";
  data["pin_set"] = this->lock_pin_hash_ != 0 ? "1" : "0";
  data["lock_display"] = this->lock_shows_screensaver_ ? "screensaver" : "keypad";
  data["settings_access"] = this->settings_access_ == 2 ? "hidden" : this->settings_access_ == 1 ? "pin" : "open";
#endif
#ifdef USE_NSPANEL_RELAYS
  JsonDocument relays;
  for (auto &relay : this->relays_) {
    JsonObject item = relays.add<JsonObject>();
    item["light"] = relay.light->get_name().c_str();
    if (relay.relay_switch != nullptr) item["switch"] = relay.relay_switch->get_name().c_str();
    if (relay.button != nullptr) item["button"] = relay.button->get_name().c_str();
  }
  serializeJson(relays, data["relays"]);
#endif
  this->fire_homeassistant_event("esphome.nspanel_lovelace_layout_status", data);
}

void NSPanelLovelace::loop_layout_status_() {
  bool connected = this->is_connected();
  if (connected && !this->layout_status_connected_) {
    // Home Assistant hasn't subscribed to events yet when the connection is first made
    this->set_timeout("layout_status", LAYOUT_STATUS_DELAY_MS, [this]() { this->report_layout_status(); });
  }
  this->layout_status_connected_ = connected;
}

} // namespace nspanel_lovelace
} // namespace esphome

#endif // USE_NSPANEL_RUNTIME_LAYOUT
