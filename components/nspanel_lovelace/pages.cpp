#include "pages.h"

#include "config.h"
#include "types.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace nspanel_lovelace {

/*
 * =============== ScreensaverEntityItem ===============
 */

std::string &ScreensaverEntityItem::render_(std::string &buffer) {
  // skip: type,internalName
  buffer.append(2, SEPARATOR);
  PageItem_Icon::render_(buffer).append(1, SEPARATOR);
  PageItem_DisplayName::render_(buffer).append(1, SEPARATOR);
  if (this->is_state(entity_state::unavailable) || this->is_state(entity_state::unknown)) {
    return buffer.append("--");
  }
  // the screensaver slots only fit a short value, numbers are shown with 1 decimal
  auto number = parse_number<float>(this->value_);
  if (number.has_value()) {
    return buffer.append(str_snprintf("%.1f", 16, *number)).append(this->value_postfix_);
  }
  return PageItem_Value::render_(buffer);
}

/*
 * =============== Screensaver ===============
 */

void Screensaver::accept(PageVisitor& visitor) { visitor.visit(*this); }

bool Screensaver::has_entity_item(const std::string &entity_id) {
  for (auto &item : this->items_) {
    auto stateful_item = page_item_cast<StatefulPageItem>(item.get());
    if (stateful_item != nullptr && stateful_item->get_entity_id() == entity_id) {
      return true;
    }
  }
  return false;
}

void Screensaver::set_icon_left(std::shared_ptr<StatusIconItem> left_icon) {
  this->left_icon = std::move(left_icon);
}
void Screensaver::set_icon_right(std::shared_ptr<StatusIconItem> right_icon) {
  this->right_icon = std::move(right_icon);
}

// output: weatherUpd~(5x)[type~internalName~icon~iconColor~displayName~value]
std::string &Screensaver::render(std::string &buffer) {
  buffer.assign(this->get_render_instruction());

  for (auto &item : this->items_) {
    buffer.append(1, SEPARATOR).append(item->render());
  }
  
  return buffer;
}

// output: statusUpdate~icon1~icon1Color~icon2~icon2Color~icon1AltFont~icon2AltFont
std::string &Screensaver::render_status_update(std::string &buffer) {
  if (!this->left_icon || !this->right_icon) {
    // buffer.clear();
    return buffer;
  }

  std::string alt_font;
  buffer.assign("statusUpdate").append(1, SEPARATOR);
  
  if (this->left_icon) {
    buffer.append(this->left_icon->render()).append(1, SEPARATOR);
    if (this->left_icon->get_alt_font()) {
      alt_font.append(1, '1');
    }
  } else {
    buffer.append(2, SEPARATOR);
  }
  alt_font.append(1, SEPARATOR);

  if (this->right_icon) {
    buffer.append(this->right_icon->render()).append(1, SEPARATOR);
    if (this->right_icon->get_alt_font()) {
      alt_font.append(1, '1');
    }
  } else {
    buffer.append(2, SEPARATOR);
  }
  
  return buffer.append(alt_font);
}

} // namespace nspanel_lovelace
} // namespace esphome