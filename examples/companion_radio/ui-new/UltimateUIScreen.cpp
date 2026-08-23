#ifdef NEONPOCKET_ULTIMATE

#include "UltimateUIScreen.h"

#include "UITask.h"
#include "../MyMesh.h"
#include "../NodePrefs.h"
#include <helpers/ContactInfo.h>
#include <helpers/ChannelDetails.h>
#include <helpers/TxtDataHelpers.h>
#include <target.h>
#ifdef RCC6_WEB_AP
  #include <helpers/esp32/SerialWebInterface.h>
  extern SerialWebInterface web_interface;
#endif

namespace {
constexpr uint32_t transition_millis = 360;
constexpr uint8_t radio_page_count = 4;
constexpr uint8_t tool_count = 9;
const char* const tool_labels[tool_count] = {
    "REPLY LATEST", "COMPOSE", "RESUME DRAFT", "ADVERTISE", "TRANSPORT",
    "HISTORY", "PRIVACY", "ABOUT", "BACK"};
const uint16_t history_options[] = {0, 128, 512, 2048};
const char* const keyboard_groups[] = {
    "abcdefg", "hijklmn", "opqrstu", "vwxyz09", " .,!?'", "^_<SX"};
constexpr uint8_t keyboard_group_count =
    sizeof(keyboard_groups) / sizeof(keyboard_groups[0]);

// Ultimate's palette is deliberately softer than the legacy primary colors.
// Every value is native RGB565 and remains exact in the indexed framebuffer.
constexpr ColorVal ultimate_ink = 0x0003;
constexpr ColorVal ultimate_card = 0x008A;
constexpr ColorVal ultimate_blue = 0x02FF;
constexpr ColorVal ultimate_cyan = 0x07FF;
constexpr ColorVal ultimate_lime = 0x87E0;
constexpr ColorVal ultimate_muted = 0x6D3A;
constexpr ColorVal ultimate_white = 0xEFFF;
constexpr ColorVal ultimate_magenta = 0xC81F;

uint16_t easeOutCubic(uint16_t value, uint16_t maximum) {
  if (value >= maximum) return maximum;
  const uint32_t inverse = maximum - value;
  const uint32_t remaining = inverse * inverse * inverse / (maximum * maximum);
  return maximum - static_cast<uint16_t>(remaining);
}

void drawListCard(DisplayDriver& display, int x, int y, int width, int height,
                  bool selected) {
  if (selected) {
    display.setColor(ultimate_card);
    display.fillRect(x + 1, y + 1, width - 2, height - 2);
  }
  display.setColor(selected ? ultimate_cyan : ultimate_blue);
  display.drawRect(x, y, width, height);
  if (selected) {
    display.setColor(ultimate_lime);
    display.fillRect(x + 3, y + 4, 3, height - 8);
  }
}

const char* deliveryLabel(UltimateDeliveryState state) {
  switch (state) {
    case UltimateDeliveryState::Queued: return "QUEUED";
    case UltimateDeliveryState::OnAir: return "ON AIR";
    case UltimateDeliveryState::Transmitted: return "TRANSMITTED";
    case UltimateDeliveryState::Acked: return "ACKED";
    case UltimateDeliveryState::NoAck: return "NO ACK";
    case UltimateDeliveryState::Unconfirmed: return "UNCONFIRMED";
    case UltimateDeliveryState::Failed: return "FAILED";
    default: return "";
  }
}

const char* powerProfileLabel(uint8_t profile) {
  switch (static_cast<UltimatePowerProfile>(profile)) {
    case UltimatePowerProfile::Field: return "FIELD";
    case UltimatePowerProfile::Battery: return "BATTERY";
    default: return "BALANCED";
  }
}
}

void UltimateUIScreen::refreshThreads() {
  thread_count = 0;
  const uint8_t available = ultimate_service.getThreadCount();
  for (uint8_t i = 0; i < available && thread_count < 32; i++) {
    const UltimateThreadSummary* source = ultimate_service.getThread(i);
    if (!source) continue;
    ThreadSummary& destination = threads[thread_count++];
    destination.kind = source->kind;
    destination.target = source->target;
    memcpy(destination.peer_key, source->peer_key, sizeof(destination.peer_key));
    StrHelper::strncpy(destination.label, source->label, sizeof(destination.label));
    destination.newest_sequence = source->newest_sequence;
    destination.unread = source->unread;
  }
  if (selection >= thread_count) selection = 0;
}

bool UltimateUIScreen::findThreadMessage(
    uint16_t ordinal, UltimateHistoryRecord& record) const {
  return ultimate_service.getThreadMessage(selected_thread.kind, selected_thread.target,
      selected_thread.peer_key, ordinal, record);
}

bool UltimateUIScreen::loadThreadMessage(uint16_t ordinal) {
  current_message_valid = findThreadMessage(ordinal, current_message);
  if (current_message_valid) {
    selected_message_sequence = current_message.sequence;
    selected_message_ordinal = ordinal;
  }
  return current_message_valid;
}

uint8_t UltimateUIScreen::buildTargets(ComposeTarget* destination, uint8_t capacity) const {
  uint8_t count = 0;
  auto add = [&](uint8_t kind, uint8_t target, const uint8_t* key, const char* label) {
    for (uint8_t i = 0; i < count; i++) {
      const bool same = destination[i].kind == kind &&
          (kind == static_cast<uint8_t>(UltimateMessageKind::Channel)
               ? destination[i].target == target
               : memcmp(destination[i].peer_key, key, 6) == 0);
      if (same) return;
    }
    if (count >= capacity) return;
    destination[count].kind = kind;
    destination[count].target = target;
    if (key) memcpy(destination[count].peer_key, key, 6);
    StrHelper::strncpy(destination[count].label, label, sizeof(destination[count].label));
    count++;
  };

  const UltimateComposerState& saved = ultimate_service.getComposerState();
  if (saved.pinned_kind) {
    add(saved.pinned_kind, saved.pinned_target, saved.pinned_key,
        saved.pinned_label);
  }

  for (uint8_t i = 0; i < ultimate_service.getThreadCount(); i++) {
    const UltimateThreadSummary* thread = ultimate_service.getThread(i);
    if (thread) add(thread->kind, thread->target, thread->peer_key, thread->label);
  }

  ChannelDetails channel;
  for (uint8_t i = 0; i < MAX_GROUP_CHANNELS && count < capacity; i++) {
    if (!the_mesh.getChannel(i, channel) || channel.name[0] == 0) continue;
    char label[32];
    snprintf(label, sizeof(label), "#%s", channel.name);
    add(static_cast<uint8_t>(UltimateMessageKind::Channel), i, nullptr, label);
  }

  ContactInfo contact;
  for (uint16_t i = MAX_ANON_CONTACTS;
       i < the_mesh.getTotalContactSlots() && count < capacity; i++) {
    if (!the_mesh.getContactByIdx(i, contact) || contact.type == ADV_TYPE_NONE ||
        contact.name[0] == 0) {
      continue;
    }
    add(static_cast<uint8_t>(UltimateMessageKind::Direct), 0, contact.id.pub_key, contact.name);
  }
  return count;
}

void UltimateUIScreen::refreshTargets() {
  memset(targets, 0, sizeof(targets));
  target_count = buildTargets(targets, 48);
  selection = 0;
}

void UltimateUIScreen::enterView(View next) {
  const View previous = view;
  if (previous != next) {
    const bool backwards = next == View::Root ||
        (previous == View::InboxMessage && next == View::InboxThreads) ||
        (previous == View::NetworkDetail && next == View::NetworkList) ||
        ((previous == View::TargetPicker || previous == View::PhrasePicker ||
          previous == View::Keyboard || previous == View::SendConfirm ||
          previous == View::HistorySettings) && next == View::ToolMenu);
    startAreaTransition(backwards ? -1 : 1);
  }
  view = next;
  selection = 0;
  message_page_offset = message_next_offset = 0;
  message_has_more = false;
  current_message_valid = false;
  history_clear_armed = false;
  if (view == View::InboxThreads) refreshThreads();
  if (view == View::TargetPicker) refreshTargets();
  task->requestRefresh();
}

void UltimateUIScreen::startAreaTransition(int8_t direction) {
  transition_direction = direction;
  transition_started = millis();
}

void UltimateUIScreen::advanceArea() {
  area = static_cast<Area>((static_cast<uint8_t>(area) + 1) %
                           static_cast<uint8_t>(Area::Count));
  view = View::Root;
  selection = 0;
  startAreaTransition(1);
}

const char* UltimateUIScreen::areaTitle() const {
  switch (area) {
    case Area::Home: return "HOME";
    case Area::Inbox: return "INBOX";
    case Area::Network: return "NETWORK";
    case Area::Radio: return "RADIO";
    case Area::Tools: return "TOOLS";
    case Area::Power: return "POWER";
    default: return "ULTIMATE";
  }
}

void UltimateUIScreen::renderHeader(DisplayDriver& display, const char* title) {
  const UltimateSnapshot& status = ultimate_service.getSnapshot();
  const uint16_t shimmer =
      (millis() / ultimate_service.getRecommendedFrameMillis()) % display.width();
  display.setColor(ultimate_ink);
  display.fillRect(0, 0, display.width(), 18);
  display.setTextSize(1);
  display.setColor(ultimate_lime);
  char node_name[32];
  display.translateUTF8ToBlocks(node_name, prefs->node_name, sizeof(node_name));
  display.drawTextEllipsized(4, 1, 102, node_name);
  display.setColor(task->hasConnection() ? ultimate_cyan : NEON_ORANGE);
  display.fillRect(112, 5, 6, 6);
  display.setColor(ultimate_white);
  display.setCursor(123, 1);
#ifdef RCC6_WEB_AP
  display.print("WEB");
#else
  display.print("BLE");
#endif
  char battery[18];
  if (status.battery_mv) {
    snprintf(battery, sizeof(battery), "%umV", status.battery_mv);
  } else {
    strcpy(battery, "--");
  }
  display.setColor(status.battery_mv && status.battery_mv <= 3450 ? NEON_RED : ultimate_lime);
  display.drawTextRightAlign(display.width() - 4, 1, battery);
  display.setColor(ultimate_blue);
  display.fillRect(0, 17, display.width(), 2);
  display.setColor(ultimate_cyan);
  display.fillRect(shimmer, 17, 12, 2);
  display.setColor(area == Area::Power ? NEON_RED : ultimate_white);
  display.drawTextCentered(display.width() / 2, 22, title);
}

void UltimateUIScreen::renderFooter(DisplayDriver& display, const char* hint) {
  int x = 8;
  for (uint8_t i = 0; i < static_cast<uint8_t>(Area::Count); i++, x += 11) {
    display.setColor(i == static_cast<uint8_t>(area) ? ultimate_lime : ultimate_blue);
    if (i == static_cast<uint8_t>(area)) display.fillRect(x - 2, 120, 5, 5);
    else display.fillRect(x, 122, 2, 2);
  }
  display.setTextSize(1);
  display.setColor(area == Area::Power ? NEON_RED : ultimate_blue);
  display.drawTextRightAlign(display.width() - 4, 115, hint);
}

void UltimateUIScreen::renderHome(DisplayDriver& display) {
  const UltimateSnapshot& status = ultimate_service.getSnapshot();
  const uint32_t newest_sequence = ultimate_service.getNewestSequence();
  if (newest_sequence != home_latest_sequence ||
      status.unread_count != home_latest_unread_count) {
    home_latest_valid = status.unread_count
        ? ultimate_service.getNewestUnread(home_latest)
        : ultimate_service.getHistoryNewest(0, home_latest);
    home_latest_sequence = newest_sequence;
    home_latest_unread_count = status.unread_count;
  } else if (newest_sequence == 0) {
    home_latest_valid = false;
  }
  display.setColor(ultimate_card);
  display.fillRect(4, 37, 212, 42);
  display.setColor(ultimate_blue);
  display.drawRect(4, 37, 212, 42);
  display.setTextSize(1);
  display.setColor(status.unread_count ? NEON_YELLOW : ultimate_lime);
  char line[64];
  snprintf(line, sizeof(line), "%u UNREAD", status.unread_count);
  display.setCursor(8, 42);
  display.print(line);
  if (task->isPrivateNotificationLocked()) {
    display.setColor(ultimate_magenta);
    display.drawTextRightAlign(212, 42, "PRIVATE ALERT");
    display.setColor(ultimate_white);
    display.drawTextCentered(110, 60, "Press once to reveal");
  } else if (home_latest_valid) {
    char sender[32];
    display.translateUTF8ToBlocks(sender, home_latest.sender, sizeof(sender));
    display.setColor(home_latest.kind == static_cast<uint8_t>(UltimateMessageKind::Channel)
                         ? ultimate_cyan : ultimate_lime);
    display.drawTextEllipsized(88, 42, 124, sender);
    display.translateUTF8ToBlocks(line, home_latest.text, sizeof(line));
    display.setColor(ultimate_white);
    display.drawTextEllipsized(8, 60, 204, line);
  } else {
    display.setColor(ultimate_muted);
    display.drawTextCentered(110, 60, "No message history");
  }

  display.setColor(ultimate_card);
  display.fillRect(4, 84, 212, 25);
  display.setColor(ultimate_blue);
  display.drawRect(4, 84, 212, 25);
  display.setColor(status.last_signal_valid ? ultimate_lime : NEON_ORANGE);
  if (status.last_signal_valid) {
    snprintf(line, sizeof(line), "RF %ddBm %.1fdB", status.last_rssi_dbm,
             status.last_snr_quarter_db / 4.0f);
  } else {
    snprintf(line, sizeof(line), "RF %.3f SF%u", prefs->freq, prefs->sf);
  }
  display.drawTextEllipsized(8, 91, 104, line);
  const UltimateDeliverySnapshot& delivery = ultimate_service.getDelivery();
  if (delivery.state != UltimateDeliveryState::Idle) {
    const bool success = delivery.state == UltimateDeliveryState::Acked ||
        delivery.state == UltimateDeliveryState::Transmitted;
    const bool failure = delivery.state == UltimateDeliveryState::Failed ||
        delivery.state == UltimateDeliveryState::NoAck;
    display.setColor(success ? ultimate_lime : (failure ? NEON_RED : NEON_YELLOW));
    char delivery_line[28];
    if (delivery.state == UltimateDeliveryState::Acked && delivery.round_trip_millis) {
      snprintf(delivery_line, sizeof(delivery_line), "TX ACK %.1fs",
               delivery.round_trip_millis / 1000.0f);
    } else {
      snprintf(delivery_line, sizeof(delivery_line), "TX %s", deliveryLabel(delivery.state));
    }
    display.drawTextRightAlign(212, 91, delivery_line);
  } else {
    display.setColor(task->hasConnection() ? ultimate_cyan : NEON_ORANGE);
    display.drawTextRightAlign(212, 91, task->hasConnection() ? "LINKED" : "STANDALONE");
  }
}

void UltimateUIScreen::renderInboxRoot(DisplayDriver& display) {
  const UltimateSnapshot& status = ultimate_service.getSnapshot();
  display.setColor(NEON_BLUE);
  display.drawRect(8, 40, 204, 61);
  display.setTextSize(2);
  display.setColor(status.unread_count ? NEON_YELLOW : NEON_GREEN);
  char count[24];
  snprintf(count, sizeof(count), "%u", status.unread_count);
  display.drawTextCentered(68, 55, count);
  display.setTextSize(1);
  display.setColor(NEON_LIGHT);
  display.drawTextCentered(68, 82, "UNREAD");
  display.setTextSize(2);
  display.setColor(NEON_BLUE);
  snprintf(count, sizeof(count), "%u", ultimate_service.getThreadCount());
  display.drawTextCentered(154, 55, count);
  display.setTextSize(1);
  display.setColor(NEON_LIGHT);
  display.drawTextCentered(154, 82, "THREADS");
}

void UltimateUIScreen::renderNetworkRoot(DisplayDriver& display) {
  display.setColor(NEON_BLUE);
  display.drawRect(8, 40, 204, 61);
  display.setTextSize(2);
  display.setColor(NEON_GREEN);
  char count[24];
  snprintf(count, sizeof(count), "%u", ultimate_service.getNetworkCount());
  display.drawTextCentered(64, 54, count);
  display.setTextSize(1);
  display.setColor(NEON_LIGHT);
  display.drawTextCentered(64, 82, "RECENT RADIOS");
  const UltimateNetworkNode* node = ultimate_service.getNetworkNode(0);
  if (node) {
    display.setColor(NEON_GREEN);
    display.drawTextEllipsized(116, 52, 88, node->name);
    display.setColor(NEON_LIGHT);
    snprintf(count, sizeof(count), "%u hop%s", node->path_len,
             node->path_len == 1 ? "" : "s");
    display.drawTextEllipsized(116, 72, 88, count);
  } else {
    display.setColor(NEON_ORANGE);
    display.drawTextCentered(160, 64, "LISTENING");
  }
}

void UltimateUIScreen::renderRadioRoot(DisplayDriver& display) {
  const UltimateSnapshot& status = ultimate_service.getSnapshot();
  display.setColor(NEON_BLUE);
  display.drawRect(4, 39, 103, 43);
  display.drawRect(113, 39, 103, 43);
  display.setTextSize(1);
  display.setColor(NEON_LIGHT);
  display.drawTextCentered(55, 44, "RX / TX");
  display.drawTextCentered(165, 44, "RSSI / SNR");
  char line[36];
  snprintf(line, sizeof(line), "%lu/%lu", (unsigned long)status.rx_packets,
           (unsigned long)status.tx_packets);
  display.setColor(NEON_GREEN);
  display.drawTextCentered(55, 63, line);
  if (status.last_signal_valid) {
    snprintf(line, sizeof(line), "%d/%.1f", status.last_rssi_dbm,
             status.last_snr_quarter_db / 4.0f);
  } else strcpy(line, "--/--");
  display.drawTextCentered(165, 63, line);
  display.setColor(NEON_LIGHT);
  snprintf(line, sizeof(line), "%.3f BW%.1f SF%u CR%u TX%d", prefs->freq, prefs->bw,
           prefs->sf, prefs->cr, prefs->tx_power_dbm);
  display.drawTextEllipsized(5, 88, 210, line);
}

void UltimateUIScreen::renderToolsRoot(DisplayDriver& display) {
  display.setColor(NEON_BLUE);
  display.drawRect(8, 40, 204, 61);
  display.setTextSize(2);
  display.setColor(NEON_GREEN);
  display.drawTextCentered(110, 49, "COMPOSE");
  display.setTextSize(1);
  display.setColor(NEON_LIGHT);
  display.drawTextCentered(110, 78, "Messages / history / transport");
}

void UltimateUIScreen::renderPowerRoot(DisplayDriver& display) {
  const UltimateSnapshot& status = ultimate_service.getSnapshot();
  const UltimateSettings& settings = ultimate_service.getSettings();
  display.setColor(status.battery_mv && status.battery_mv <= 3450 ? NEON_RED : ultimate_blue);
  display.drawRect(8, 39, 204, 70);
  display.setTextSize(1);
  display.setColor(status.battery_mv && status.battery_mv <= 3450 ? NEON_RED : ultimate_lime);
  char line[48];
  if (status.battery_mv) {
    snprintf(line, sizeof(line), "BATTERY %.2fV", status.battery_mv / 1000.0f);
  } else {
    strcpy(line, "BATTERY --");
  }
  display.setCursor(13, 45); display.print(line);
  display.setColor(ultimate_cyan);
  display.drawTextRightAlign(207, 45, powerProfileLabel(settings.power_profile));
  display.setColor(ultimate_white);
  if (status.usb_host_connected) {
    strcpy(line, "USB HOST CONNECTED");
  } else if (status.battery_projection_valid) {
    snprintf(line, sizeof(line), "TREND %+d mV/h", status.battery_trend_mv_per_hour);
  } else {
    strcpy(line, "TREND LEARNING");
  }
  display.setCursor(13, 62); display.print(line);
  if (settings.battery_capacity_mah) {
    snprintf(line, sizeof(line), "%umAh", settings.battery_capacity_mah);
    display.setColor(ultimate_cyan);
    display.drawTextRightAlign(207, 62, line);
  }
  if (status.usb_host_connected) {
    strcpy(line, "UNPLUG TO START LEARNING");
  } else if (status.battery_runtime_minutes) {
    snprintf(line, sizeof(line), "EST ~%uh %02um TO 3.45V",
             status.battery_runtime_minutes / 60, status.battery_runtime_minutes % 60);
  } else if (status.battery_projection_valid && status.battery_trend_mv_per_hour >= 3) {
    strcpy(line, "RISING / USB OR CHARGING");
  } else {
    strcpy(line, "RUNTIME ESTIMATE PENDING");
  }
  display.setColor(ultimate_muted);
  display.setCursor(13, 79); display.print(line);
  display.setColor(task->isPowerConfirmArmed() ? NEON_RED : ultimate_blue);
  display.drawTextCentered(110, 96, shutdown_pending ? "POWERING OFF" :
      (task->isPowerConfirmArmed() ? "HOLD AGAIN TO CONFIRM" : "2X PROFILE  HOLD POWER"));
}

void UltimateUIScreen::renderRoot(DisplayDriver& display) {
  renderHeader(display, areaTitle());
  switch (area) {
    case Area::Home: renderHome(display); break;
    case Area::Inbox: renderInboxRoot(display); break;
    case Area::Network: renderNetworkRoot(display); break;
    case Area::Radio: renderRadioRoot(display); break;
    case Area::Tools: renderToolsRoot(display); break;
    case Area::Power: renderPowerRoot(display); break;
    default: break;
  }
  const char* hint = area == Area::Power ? "HOLD TO ARM  3X HOME" :
      (area == Area::Home ? "CLICK NEXT  2X OPEN" : "1X NEXT  2X OPEN  3X HOME");
  renderFooter(display, hint);
}

void UltimateUIScreen::renderInboxThreads(DisplayDriver& display) {
  renderHeader(display, "INBOX THREADS");
  if (thread_count == 0) {
    display.setColor(NEON_LIGHT);
    display.drawTextCentered(110, 66, "No stored messages");
  } else {
    const uint8_t total = thread_count + 1;
    const uint8_t first = selection > 2 ? selection - 2 : 0;
    for (uint8_t row = 0; row < 4 && first + row < total; row++) {
      const uint8_t index = first + row;
      const int y = 38 + row * 18;
      drawListCard(display, 5, y, 210, 16, index == selection);
      if (index == thread_count) {
        display.setColor(NEON_RED);
        display.drawTextCentered(110, y + 3, "BACK TO HOME");
        continue;
      }
      const ThreadSummary& thread = threads[index];
      display.setColor(thread.kind == static_cast<uint8_t>(UltimateMessageKind::Channel)
                           ? NEON_BLUE : NEON_GREEN);
      display.drawTextEllipsized(14, y + 3, 150, thread.label);
      if (thread.unread) {
        char unread[14];
        snprintf(unread, sizeof(unread), "%u new", thread.unread);
        display.setColor(NEON_YELLOW);
        display.drawTextRightAlign(211, y + 3, unread);
      }
    }
  }
  renderFooter(display, thread_count ? "1X NEXT  2X OPEN  3X CLEAR" : "2X BACK  3X CLEAR");
}

size_t UltimateUIScreen::renderMessagePage(
    DisplayDriver& display, const char* text, size_t offset) const {
  constexpr size_t chars_per_line = 33;
  constexpr int line_count = 4;
  constexpr int first_y = 49;
  constexpr int line_height = 15;
  const size_t length = strlen(text);
  size_t position = offset < length ? offset : 0;
  for (int line_index = 0; line_index < line_count && position < length; line_index++) {
    while (position < length && (text[position] == ' ' || text[position] == '\t' ||
                                 text[position] == '\r')) position++;
    if (position >= length) break;
    if (text[position] == '\n') {
      position++;
      continue;
    }
    const size_t start = position;
    size_t end = start;
    while (end < length && end - start < chars_per_line && text[end] != '\n' &&
           text[end] != '\r') end++;
    size_t next = end;
    if (end < length && text[end] != '\n' && text[end] != '\r') {
      size_t split = end;
      while (split > start && text[split - 1] != ' ') split--;
      if (split > start) {
        end = split - 1;
        next = split;
      }
    } else if (end < length) next = end + 1;
    char line[chars_per_line + 1];
    const size_t count = end - start;
    memcpy(line, text + start, count);
    line[count] = 0;
    display.setCursor(10, first_y + line_index * line_height);
    display.print(line);
    position = next;
  }
  return position;
}

void UltimateUIScreen::renderInboxMessage(DisplayDriver& display) {
  if (!current_message_valid && !loadThreadMessage(selected_message_ordinal)) {
    enterView(View::InboxThreads);
    renderInboxThreads(display);
    return;
  }
  if ((current_message.flags & (ULTIMATE_HISTORY_INCOMING | ULTIMATE_HISTORY_READ)) ==
      ULTIMATE_HISTORY_INCOMING) {
    if (ultimate_service.markRead(current_message.sequence)) {
      current_message.flags |= ULTIMATE_HISTORY_READ;
    }
  }
  renderHeader(display, current_message.sender);
  display.setColor(current_message.kind == static_cast<uint8_t>(UltimateMessageKind::Channel)
                       ? NEON_BLUE : NEON_YELLOW);
  char meta_line[48];
  const UltimateDeliverySnapshot& delivery = ultimate_service.getDelivery();
  if ((current_message.flags & ULTIMATE_HISTORY_INCOMING) == 0 &&
      delivery.history_sequence == current_message.sequence &&
      delivery.state != UltimateDeliveryState::Idle) {
    snprintf(meta_line, sizeof(meta_line), "TX %s  %u hop%s",
             deliveryLabel(delivery.state),
             current_message.path_len == 0xFF ? 0 : current_message.path_len,
             current_message.path_len == 1 ? "" : "s");
  } else {
    snprintf(meta_line, sizeof(meta_line), "%s  %u hop%s",
             (current_message.flags & ULTIMATE_HISTORY_INCOMING) ? "RECEIVED" : "SENT",
             current_message.path_len == 0xFF ? 0 : current_message.path_len,
             current_message.path_len == 1 ? "" : "s");
  }
  display.setCursor(5, 34);
  display.print(meta_line);
  display.setColor(NEON_BLUE);
  display.drawRect(4, 44, 212, 65);
  display.setColor(NEON_LIGHT);
  char filtered[sizeof(current_message.text)];
  display.translateUTF8ToBlocks(filtered, current_message.text, sizeof(filtered));
  message_next_offset = renderMessagePage(display, filtered, message_page_offset);
  message_has_more = filtered[message_next_offset] != 0;
  renderFooter(display, message_has_more ? "1X MORE  2X BACK  3X CLEAR" :
      "1X NEXT  2X BACK  3X CLEAR");
}

void UltimateUIScreen::renderNetworkList(DisplayDriver& display) {
  renderHeader(display, "NETWORK EXPLORER");
  const uint8_t count = ultimate_service.getNetworkCount();
  if (count == 0) {
    display.setColor(NEON_LIGHT);
    display.drawTextCentered(110, 66, "Listening on current preset");
  } else {
    const uint8_t total = count + 1;
    const uint8_t first = selection > 2 ? selection - 2 : 0;
    for (uint8_t row = 0; row < 4 && first + row < total; row++) {
      const uint8_t index = first + row;
      const int y = 38 + row * 18;
      drawListCard(display, 5, y, 210, 16, index == selection);
      if (index == count) {
        display.setColor(NEON_RED);
        display.drawTextCentered(110, y + 3, "BACK TO HOME");
        continue;
      }
      const UltimateNetworkNode* node = ultimate_service.getNetworkNode(index);
      if (!node) continue;
      display.setColor(NEON_GREEN);
      display.drawTextEllipsized(14, y + 3, 140, node->name);
      char value[20];
      if (node->signal_attributable) snprintf(value, sizeof(value), "%ddBm", node->rssi_dbm);
      else snprintf(value, sizeof(value), "%u hop%s", node->path_len,
                    node->path_len == 1 ? "" : "s");
      display.setColor(node->signal_attributable ? NEON_LIGHT : NEON_ORANGE);
      display.drawTextRightAlign(211, y + 3, value);
    }
  }
  renderFooter(display, count ? "CLICK NEXT  2X SELECT" : "2X BACK");
}

void UltimateUIScreen::renderNetworkDetail(DisplayDriver& display) {
  const UltimateNetworkNode* node = ultimate_service.getNetworkNode(selection);
  if (!node) {
    enterView(View::NetworkList);
    renderNetworkList(display);
    return;
  }
  renderHeader(display, node->name);
  char line[64];
  display.setColor(NEON_BLUE);
  display.drawRect(8, 39, 204, 66);
  display.setColor(NEON_LIGHT);
  snprintf(line, sizeof(line), "ROLE %u   PATH %u hop%s", node->role,
           node->path_len, node->path_len == 1 ? "" : "s");
  display.setCursor(13, 45);
  display.print(line);
  const uint32_t now = rtc->getCurrentTime();
  const uint32_t age = now >= node->last_seen ? now - node->last_seen : 0;
  snprintf(line, sizeof(line), "PACKETS %lu   AGE %lus", (unsigned long)node->packet_count,
           (unsigned long)age);
  display.setCursor(13, 62);
  display.print(line);
  if (node->signal_attributable) {
    display.setColor(NEON_GREEN);
    snprintf(line, sizeof(line), "RSSI %ddBm   SNR %.1fdB", node->rssi_dbm,
             node->snr_quarter_db / 4.0f);
  } else {
    display.setColor(NEON_ORANGE);
    strcpy(line, "SIGNAL not attributable");
  }
  display.setCursor(13, 80);
  display.print(line);
  renderFooter(display, "2X BACK");
}

void UltimateUIScreen::renderRadioPages(DisplayDriver& display) {
  const UltimateSnapshot& status = ultimate_service.getSnapshot();
  const char* titles[] = {"RADIO LIVE", "TWO HOUR TRAFFIC", "SEVEN DAY TRAFFIC", "SYSTEM"};
  renderHeader(display, titles[radio_page]);
  char line[72];
  if (radio_page == 0) {
    display.setColor(NEON_BLUE);
    display.drawRect(5, 39, 210, 66);
    display.setColor(NEON_LIGHT);
    snprintf(line, sizeof(line), "RX %lu TX %lu FAIL %lu AIR %lus", (unsigned long)status.rx_packets,
             (unsigned long)status.tx_packets, (unsigned long)status.tx_failures,
             (unsigned long)(status.airtime_ms / 1000));
    display.setCursor(10, 45); display.print(line);
    if (status.last_signal_valid) {
      snprintf(line, sizeof(line), "RSSI %d  SNR %.1f  Q %u", status.last_rssi_dbm,
               status.last_snr_quarter_db / 4.0f, status.outbound_queue_depth);
    } else {
      snprintf(line, sizeof(line), "RSSI --  SNR --  Q %u", status.outbound_queue_depth);
    }
    display.setCursor(10, 62); display.print(line);
    snprintf(line, sizeof(line), "NF %d  %.3f BW%.1f SF%u", radio_driver.getNoiseFloor(),
             prefs->freq, prefs->bw, prefs->sf);
    display.setCursor(10, 79); display.print(line);
  } else if (radio_page == 1) {
    display.setColor(NEON_BLUE);
    display.drawRect(8, 42, 204, 59);
    uint16_t max_value = 1;
    uint16_t values[24] = {};
    for (uint8_t i = 0; i < 24; i++) {
      uint32_t timestamp; uint16_t rx, tx, failures, battery; int16_t rssi;
      int8_t snr; uint8_t queue;
      if (ultimate_service.getHighResolutionNewest(i * 5, timestamp, rx, tx, failures,
                                                   rssi, snr, queue, battery)) {
        values[23 - i] = rx + tx;
        if (values[23 - i] > max_value) max_value = values[23 - i];
      }
    }
    for (uint8_t i = 0; i < 24; i++) {
      const int height = values[i] * 48 / max_value;
      display.setColor(values[i] ? NEON_GREEN : NEON_BLUE);
      display.fillRect(12 + i * 8, 96 - height, 5, height + 1);
    }
  } else if (radio_page == 2) {
    display.setColor(NEON_BLUE);
    display.drawRect(8, 42, 204, 59);
    uint32_t values[28] = {};
    uint32_t max_value = 1;
    for (uint8_t day_part = 0; day_part < 28; day_part++) {
      for (uint8_t hour = 0; hour < 6; hour++) {
        const UltimateMetricBucket* bucket =
            ultimate_service.getHourlyMetricNewest(day_part * 6 + hour);
        if (bucket) values[27 - day_part] += bucket->rx_packets + bucket->tx_packets;
      }
      if (values[27 - day_part] > max_value) max_value = values[27 - day_part];
    }
    for (uint8_t i = 0; i < 28; i++) {
      const int height = values[i] * 48 / max_value;
      display.setColor(values[i] ? NEON_BLUE : NEON_DARK);
      display.fillRect(11 + i * 7, 96 - height, 4, height + 1);
    }
  } else {
    display.setColor(NEON_BLUE);
    display.drawRect(5, 39, 210, 66);
    display.setColor(NEON_LIGHT);
    snprintf(line, sizeof(line), "HEAP %luK  MAX %luK  GATE %s",
             (unsigned long)(status.free_heap / 1024),
             (unsigned long)(status.largest_allocation / 1024),
             status.memory_gate_passed ? "OK" : "WAIT");
    display.setCursor(10, 45); display.print(line);
    snprintf(line, sizeof(line), "STORE %lu/%luK  HIST %u/%u",
             (unsigned long)status.storage_used_kb, (unsigned long)status.storage_total_kb,
             status.history_count, status.history_capacity);
    display.setCursor(10, 62); display.print(line);
    snprintf(line, sizeof(line), "TFT %luus %u/176  DROP %lu",
             (unsigned long)status.display_flush_micros, status.display_tiles_sent,
             (unsigned long)status.event_drops);
    display.setCursor(10, 79); display.print(line);
  }
  renderFooter(display, "CLICK PAGE  2X BACK");
}

void UltimateUIScreen::renderToolMenu(DisplayDriver& display) {
  renderHeader(display, "TOOLS");
  const uint8_t first = selection > 2 ? selection - 2 : 0;
  for (uint8_t row = 0; row < 4 && first + row < tool_count; row++) {
    const uint8_t index = first + row;
    const int y = 38 + row * 18;
    drawListCard(display, 12, y, 196, 16, index == selection);
    display.setColor(index == selection ? NEON_YELLOW : NEON_LIGHT);
    display.drawTextCentered(110, y + 3, tool_labels[index]);
  }
  renderFooter(display, "CLICK NEXT  2X SELECT");
}

void UltimateUIScreen::renderTargetPicker(DisplayDriver& display) {
  renderHeader(display, "MESSAGE TARGET");
  if (target_count == 0) {
    display.setColor(NEON_ORANGE);
    display.drawTextCentered(110, 64, "No contacts or channels");
  } else {
    const uint8_t total = target_count + 1;
    const uint8_t first = selection > 2 ? selection - 2 : 0;
    for (uint8_t row = 0; row < 4 && first + row < total; row++) {
      const uint8_t index = first + row;
      const int y = 38 + row * 18;
      drawListCard(display, 7, y, 206, 16, index == selection);
      if (index == target_count) {
        display.setColor(NEON_RED);
        display.drawTextCentered(110, y + 3, "BACK TO TOOLS");
        continue;
      }
      display.setColor(targets[index].kind == static_cast<uint8_t>(UltimateMessageKind::Channel)
                           ? NEON_BLUE : NEON_GREEN);
      display.drawTextEllipsized(16, y + 3, 154, targets[index].label);
      if (isPinnedTarget(targets[index])) {
        display.setColor(NEON_YELLOW);
        display.drawTextRightAlign(207, y + 3, "PIN");
      }
    }
  }
  renderFooter(display, target_count ? "CLICK NEXT  2X SELECT" : "2X BACK");
}

void UltimateUIScreen::renderPhrasePicker(DisplayDriver& display) {
  renderHeader(display, compose_target.label);
  const UltimateSettings& settings = ultimate_service.getSettings();
  const uint8_t total = 11;
  const uint8_t first = selection > 2 ? selection - 2 : 0;
  for (uint8_t row = 0; row < 4 && first + row < total; row++) {
    const uint8_t index = first + row;
    const int y = 38 + row * 18;
    drawListCard(display, 5, y, 210, 16, index == selection);
    display.setColor(index == 8 ? NEON_GREEN : (index == 10 ? NEON_RED : NEON_LIGHT));
    const char* label = index < 8 ? settings.quick_phrases[index] :
        (index == 8 ? "CUSTOM KEYBOARD" :
         (index == 9 ? (isPinnedTarget(compose_target) ? "UNPIN TARGET" : "PIN TARGET")
                     : "CANCEL"));
    display.drawTextEllipsized(14, y + 3, 196, label);
  }
  renderFooter(display, "CLICK NEXT  2X SELECT");
}

void UltimateUIScreen::renderKeyboard(DisplayDriver& display) {
  renderHeader(display, compose_target.label);
  display.setColor(NEON_BLUE);
  display.drawRect(5, 37, 210, 30);
  display.setColor(NEON_LIGHT);
  display.drawTextEllipsized(9, 43, 202, compose_text[0] ? compose_text : "Type message...");
  char bytes[20];
  snprintf(bytes, sizeof(bytes), "%u/140 bytes", compose_length);
  display.setColor(compose_length >= 140 ? NEON_RED : NEON_GREEN);
  display.drawTextRightAlign(211, 55, bytes);

  const char* group = keyboard_groups[scan_group];
  display.setColor(NEON_BLUE);
  display.drawRect(5, 73, 210, 34);
  if (scan_phase == ScanPhase::Group) {
    char label[48];
    if (scan_group < 5) {
      snprintf(label, sizeof(label), "%s%s", uppercase ? "UPPER " : "", group);
    } else strcpy(label, "EDIT / SEND");
    display.setTextSize(2);
    display.setColor(NEON_YELLOW);
    display.drawTextCentered(110, 80, label);
  } else {
    char value[20];
    if (scan_group < 5) {
      value[0] = group[scan_item];
      if (uppercase && value[0] >= 'a' && value[0] <= 'z') value[0] -= 32;
      value[1] = 0;
    } else {
      const char* names[] = {"CASE", "SPACE", "BACK", "SEND", "CANCEL"};
      StrHelper::strncpy(value, names[scan_item], sizeof(value));
    }
    display.setTextSize(2);
    display.setColor(NEON_GREEN);
    display.drawTextCentered(110, 80, value);
  }
  renderFooter(display, "CLICK ADVANCE  2X SELECT");
}

void UltimateUIScreen::renderSendConfirm(DisplayDriver& display) {
  renderHeader(display, "CONFIRM SEND");
  display.setColor(NEON_BLUE);
  display.drawRect(7, 38, 206, 66);
  display.setColor(NEON_GREEN);
  display.drawTextEllipsized(12, 40, 146, compose_target.label);
  char bytes[18];
  snprintf(bytes, sizeof(bytes), "%u bytes", compose_length);
  display.setColor(NEON_YELLOW);
  display.drawTextRightAlign(207, 40, bytes);
  display.setColor(NEON_LIGHT);
  char filtered[sizeof(compose_text)];
  display.translateUTF8ToBlocks(filtered, compose_text, sizeof(filtered));
  message_next_offset = renderMessagePage(display, filtered, message_page_offset);
  message_has_more = filtered[message_next_offset] != 0;
  renderFooter(display, message_has_more ? "CLICK MORE  2X SEND" : "CLICK EDIT  2X SEND");
}

void UltimateUIScreen::renderHistorySettings(DisplayDriver& display) {
  renderHeader(display, "HISTORY STORAGE");
  const char* labels[] = {"OFF", "128 MESSAGES", "512 MESSAGES", "2048 MESSAGES",
                          "CLEAR HISTORY", "BACK"};
  const uint8_t total = 6;
  const uint8_t first = selection > 2 ? selection - 2 : 0;
  for (uint8_t row = 0; row < 4 && first + row < total; row++) {
    const uint8_t index = first + row;
    const int y = 38 + row * 18;
    drawListCard(display, 8, y, 204, 16, index == selection);
    display.setColor(index == 4 ? NEON_RED : NEON_LIGHT);
    char label[40];
    if (index < 4 && history_options[index] == ultimate_service.getSettings().history_capacity) {
      snprintf(label, sizeof(label), "%s  ACTIVE", labels[index]);
    } else if (index == 4 && history_clear_armed) {
      strcpy(label, "2X AGAIN TO ERASE");
    } else StrHelper::strncpy(label, labels[index], sizeof(label));
    display.drawTextCentered(110, y + 3, label);
  }
  renderFooter(display, "CLICK NEXT  2X SELECT");
}

void UltimateUIScreen::renderTransition(DisplayDriver& display) {
  if (!transition_started) return;
  const uint32_t elapsed = millis() - transition_started;
  if (elapsed >= transition_millis) {
    transition_started = 0;
    transition_direction = 0;
    return;
  }
  const uint16_t eased = easeOutCubic(elapsed, transition_millis);
  const int travel = (display.width() + 18) * eased / transition_millis;
  const int x = transition_direction > 0 ? travel - 9 : display.width() + 8 - travel;
  const uint16_t frame_millis = ultimate_service.getRecommendedFrameMillis();

  // A tight scan beam and deterministic orbiting sparks keep each animation
  // frame local enough for the 20x8 tile flusher to sustain 15 FPS.
  display.setColor(ultimate_magenta);
  display.fillRect(x - 7, 23, 2, 88);
  display.setColor(ultimate_blue);
  display.fillRect(x - 4, 20, 3, 94);
  display.setColor(ultimate_cyan);
  display.fillRect(x, 19, 2, 96);
  display.setColor(ultimate_white);
  display.fillRect(x + 2, 30, 1, 74);
  display.setColor(ultimate_lime);
  display.fillRect(x + 5, 27 + ((elapsed / frame_millis) % 4) * 19, 3, 3);
  display.fillRect(x - 10, 99 - ((elapsed / frame_millis) % 3) * 23, 2, 2);
}

int UltimateUIScreen::render(DisplayDriver& display) {
  switch (view) {
    case View::Root: renderRoot(display); break;
    case View::InboxThreads: renderInboxThreads(display); break;
    case View::InboxMessage: renderInboxMessage(display); break;
    case View::NetworkList: renderNetworkList(display); break;
    case View::NetworkDetail: renderNetworkDetail(display); break;
    case View::RadioPages: renderRadioPages(display); break;
    case View::ToolMenu: renderToolMenu(display); break;
    case View::TargetPicker: renderTargetPicker(display); break;
    case View::PhrasePicker: renderPhrasePicker(display); break;
    case View::Keyboard: renderKeyboard(display); break;
    case View::SendConfirm: renderSendConfirm(display); break;
    case View::HistorySettings: renderHistorySettings(display); break;
  }
  renderTransition(display);
  // Preserve 15 FPS when the device has headroom; the service can reduce the
  // cadence under display, queue, heap, or battery pressure.
  return ultimate_service.getRecommendedFrameMillis();
}

void UltimateUIScreen::appendComposer(char value) {
  if (compose_length >= 140) return;
  compose_text[compose_length++] = value;
  compose_text[compose_length] = 0;
  markDraftDirty();
}

bool UltimateUIScreen::isPinnedTarget(const ComposeTarget& target) const {
  const UltimateComposerState& saved = ultimate_service.getComposerState();
  if (saved.pinned_kind != target.kind || saved.pinned_target != target.target) return false;
  return target.kind == static_cast<uint8_t>(UltimateMessageKind::Channel) ||
      memcmp(saved.pinned_key, target.peer_key, sizeof(target.peer_key)) == 0;
}

void UltimateUIScreen::startCompose(const ComposeTarget& target, bool restore_draft) {
  compose_target = target;
  if (restore_draft) {
    const UltimateComposerState& saved = ultimate_service.getComposerState();
    const bool same = saved.draft_kind == target.kind && saved.draft_target == target.target &&
        (target.kind == static_cast<uint8_t>(UltimateMessageKind::Channel) ||
         memcmp(saved.draft_key, target.peer_key, sizeof(target.peer_key)) == 0);
    if (same) {
      StrHelper::strncpy(compose_text, saved.draft_text, sizeof(compose_text));
      compose_length = strlen(compose_text);
    } else {
      compose_text[0] = 0;
      compose_length = 0;
    }
  }
  enterView(View::PhrasePicker);
}

void UltimateUIScreen::markDraftDirty() {
  draft_dirty = true;
  draft_save_due = millis() + 3000U;
}

void UltimateUIScreen::persistDraft() {
  if (!draft_dirty) return;
  if (compose_text[0]) {
    ultimate_service.saveDraft(compose_target.kind, compose_target.target,
        compose_target.peer_key, compose_target.label, compose_text);
  } else {
    ultimate_service.clearDraft();
  }
  draft_dirty = false;
  draft_save_due = 0;
}

bool UltimateUIScreen::restoreDraft() {
  const UltimateComposerState& saved = ultimate_service.getComposerState();
  if (!saved.draft_kind || !saved.draft_text[0]) return false;
  compose_target = {};
  compose_target.kind = saved.draft_kind;
  compose_target.target = saved.draft_target;
  memcpy(compose_target.peer_key, saved.draft_key, sizeof(compose_target.peer_key));
  StrHelper::strncpy(compose_target.label, saved.draft_label, sizeof(compose_target.label));
  StrHelper::strncpy(compose_text, saved.draft_text, sizeof(compose_text));
  compose_length = strlen(compose_text);
  scan_phase = ScanPhase::Group;
  scan_group = scan_item = 0;
  next_scan = millis() + ultimate_service.getSettings().scan_cadence_ms;
  enterView(View::Keyboard);
  return true;
}

void UltimateUIScreen::expandPhrase(const char* source) {
  compose_text[0] = 0;
  compose_length = 0;
  const UltimateSnapshot& status = ultimate_service.getSnapshot();
  while (source && *source && compose_length < 140) {
    char replacement[48] = {};
    size_t token_length = 0;
    if (strncmp(source, "{battery}", 9) == 0) {
      if (status.battery_mv) {
        snprintf(replacement, sizeof(replacement), "%.2fV", status.battery_mv / 1000.0f);
      } else {
        strcpy(replacement, "BATTERY UNAVAILABLE");
      }
      token_length = 9;
    } else if (strncmp(source, "{location}", 10) == 0) {
      if (prefs->node_lat != 0 || prefs->node_lon != 0) {
        snprintf(replacement, sizeof(replacement), "%.5f,%.5f",
                 prefs->node_lat, prefs->node_lon);
      } else {
        strcpy(replacement, "LOCATION UNSET");
      }
      token_length = 10;
    } else if (strncmp(source, "{name}", 6) == 0) {
      StrHelper::strncpy(replacement, prefs->node_name, sizeof(replacement));
      token_length = 6;
    }
    if (token_length) {
      for (const char* value = replacement; *value && compose_length < 140; value++) {
        compose_text[compose_length++] = *value;
      }
      source += token_length;
    } else {
      compose_text[compose_length++] = *source++;
    }
  }
  compose_text[compose_length] = 0;
  markDraftDirty();
}

void UltimateUIScreen::handleKeyboardSelect() {
  const char* group = keyboard_groups[scan_group];
  if (scan_phase == ScanPhase::Group) {
    scan_phase = ScanPhase::Item;
    scan_item = 0;
  } else if (scan_group < 5) {
    char value = group[scan_item];
    if (uppercase && value >= 'a' && value <= 'z') value -= 32;
    appendComposer(value);
    scan_phase = ScanPhase::Group;
  } else {
    switch (scan_item) {
      case 0: uppercase = !uppercase; break;
      case 1: appendComposer(' '); break;
      case 2:
        if (compose_length) {
          compose_text[--compose_length] = 0;
          markDraftDirty();
        }
        break;
      case 3:
        if (compose_length) enterView(View::SendConfirm);
        return;
      case 4: persistDraft(); enterView(View::ToolMenu); return;
    }
    scan_phase = ScanPhase::Group;
  }
  next_scan = millis() + ultimate_service.getSettings().scan_cadence_ms;
  task->requestRefresh();
}

bool UltimateUIScreen::sendComposer() {
  if (compose_length == 0) return false;
  if (compose_target.kind == static_cast<uint8_t>(UltimateMessageKind::Channel)) {
    return the_mesh.sendUltimateChannel(compose_target.target, compose_text);
  }
  return the_mesh.sendUltimateDirect(compose_target.peer_key, compose_text);
}

void UltimateUIScreen::handleToolAction() {
  if (selection == 0) {
    refreshThreads();
    if (thread_count == 0) {
      task->showAlert("No recent conversation", 1000, NEON_ORANGE);
      return;
    }
    ComposeTarget target = {};
    target.kind = threads[0].kind;
    target.target = threads[0].target;
    memcpy(target.peer_key, threads[0].peer_key, sizeof(target.peer_key));
    StrHelper::strncpy(target.label, threads[0].label, sizeof(target.label));
    startCompose(target, true);
  } else if (selection == 1) {
    enterView(View::TargetPicker);
  } else if (selection == 2) {
    if (!restoreDraft()) task->showAlert("No saved draft", 1000, NEON_ORANGE);
  } else if (selection == 3) {
    const bool queued = the_mesh.advert(true);
    if (queued) task->armManualAdvert();
    task->showAlert(queued ? "Mesh advert queued" : "Advert busy", 1000,
                    queued ? NEON_YELLOW : NEON_RED);
  } else if (selection == 4) {
#ifdef RCC6_WEB_AP
    if (web_interface.isStationMode()) {
      task->showAlert(web_interface.selectSetupAp() ? "Restarting setup AP" : "Setup failed",
                      1000, NEON_ORANGE);
    } else if (task->isSerialEnabled()) task->disableSerial();
    else task->enableSerial();
#else
    if (task->isBluetoothEnabled()) task->disableBluetooth();
    else task->enableBluetooth();
#endif
  } else if (selection == 5) {
    enterView(View::HistorySettings);
  } else if (selection == 6) {
    UltimateSettings updated = ultimate_service.getSettings();
    updated.private_notifications = !updated.private_notifications;
    const bool saved = ultimate_service.updateSettings(updated);
    task->showAlert(saved ? (updated.private_notifications ? "Private alerts on" : "Previews on")
                          : "Settings failed", 1000, saved ? NEON_GREEN : NEON_RED);
  } else if (selection == 7) {
    task->showAlert("Ultimate 2.3 RC2 / MC 1.17.0+RX", 1200, NEON_GREEN);
  } else {
    enterView(View::Root);
  }
}

void UltimateUIScreen::handleRootAction() {
  switch (area) {
    case Area::Home:
    case Area::Inbox: enterView(View::InboxThreads); break;
    case Area::Network: enterView(View::NetworkList); break;
    case Area::Radio: enterView(View::RadioPages); break;
    case Area::Tools: enterView(View::ToolMenu); break;
    case Area::Power: {
      UltimateSettings updated = ultimate_service.getSettings();
      updated.power_profile = (updated.power_profile + 1) % 3;
      const bool saved = ultimate_service.updateSettings(updated);
      char alert[32];
      snprintf(alert, sizeof(alert), "%s power profile",
               powerProfileLabel(updated.power_profile));
      task->showAlert(saved ? alert : "Profile save failed", 1000,
                      saved ? NEON_GREEN : NEON_RED);
      break;
    }
    default: break;
  }
}

bool UltimateUIScreen::handleInput(char input) {
  if (input == KEY_NEXT || input == KEY_RIGHT) {
    if (view == View::Root) {
      advanceArea();
    } else if (view == View::InboxThreads) {
      selection = (selection + 1) % (thread_count + 1);
    } else if (view == View::InboxMessage) {
      if (message_has_more) {
        message_page_offset = message_next_offset;
      } else {
        const uint16_t next_ordinal = selected_message_ordinal + 1;
        message_page_offset = message_next_offset = 0;
        current_message_valid = false;
        if (!loadThreadMessage(next_ordinal)) loadThreadMessage(0);
      }
    } else if (view == View::NetworkList) {
      const uint8_t count = ultimate_service.getNetworkCount();
      selection = (selection + 1) % (count + 1);
    } else if (view == View::RadioPages) {
      radio_page = (radio_page + 1) % radio_page_count;
    } else if (view == View::ToolMenu) {
      selection = (selection + 1) % tool_count;
    } else if (view == View::TargetPicker) {
      selection = (selection + 1) % (target_count + 1);
    } else if (view == View::PhrasePicker) {
      selection = (selection + 1) % 11;
    } else if (view == View::Keyboard) {
      if (scan_phase == ScanPhase::Group) scan_group = (scan_group + 1) % keyboard_group_count;
      else {
        const uint8_t count = scan_group < 5 ? strlen(keyboard_groups[scan_group]) : 5;
        scan_item = (scan_item + 1) % count;
      }
      next_scan = millis() + ultimate_service.getSettings().scan_cadence_ms;
    } else if (view == View::SendConfirm) {
      if (message_has_more) {
        message_page_offset = message_next_offset;
      } else {
        enterView(View::Keyboard);
      }
    } else if (view == View::HistorySettings) {
      history_clear_armed = false;
      selection = (selection + 1) % 6;
    }
    task->requestRefresh();
    return true;
  }

  if (input != KEY_ENTER) return false;
  if (view == View::Root) {
    handleRootAction();
  } else if (view == View::InboxThreads) {
    if (selection < thread_count) {
      selected_thread = threads[selection];
      selected_message_ordinal = 0;
      enterView(View::InboxMessage);
      loadThreadMessage(0);
    } else enterView(View::Root);
  } else if (view == View::InboxMessage) {
    refreshThreads();
    enterView(View::InboxThreads);
  } else if (view == View::NetworkList) {
    if (selection < ultimate_service.getNetworkCount()) {
      const uint8_t selected_node = selection;
      enterView(View::NetworkDetail);
      selection = selected_node;
    }
    else enterView(View::Root);
  } else if (view == View::NetworkDetail || view == View::RadioPages) {
    enterView(view == View::NetworkDetail ? View::NetworkList : View::Root);
  } else if (view == View::ToolMenu) {
    handleToolAction();
  } else if (view == View::TargetPicker) {
    if (selection < target_count) {
      startCompose(targets[selection], true);
    } else enterView(View::ToolMenu);
  } else if (view == View::PhrasePicker) {
    if (selection < 8) {
      expandPhrase(ultimate_service.getSettings().quick_phrases[selection]);
      enterView(View::SendConfirm);
    } else if (selection == 8) {
      scan_phase = ScanPhase::Group;
      scan_group = scan_item = 0;
      next_scan = millis() + ultimate_service.getSettings().scan_cadence_ms;
      enterView(View::Keyboard);
    } else if (selection == 9) {
      const bool pinned = isPinnedTarget(compose_target);
      const bool saved = pinned ? ultimate_service.clearPinnedTarget() :
          ultimate_service.setPinnedTarget(compose_target.kind, compose_target.target,
              compose_target.peer_key, compose_target.label);
      task->showAlert(saved ? (pinned ? "Target unpinned" : "Target pinned")
                            : "Pin save failed", 900, saved ? NEON_GREEN : NEON_RED);
    } else {
      persistDraft();
      enterView(View::ToolMenu);
    }
  } else if (view == View::Keyboard) {
    handleKeyboardSelect();
  } else if (view == View::SendConfirm) {
    const bool sent = sendComposer();
    if (sent) {
      ultimate_service.clearDraft();
      compose_text[0] = 0;
      compose_length = 0;
      draft_dirty = false;
      draft_save_due = 0;
    }
    task->showAlert(sent ? "Message queued" : "Send failed", 1200,
                    sent ? NEON_GREEN : NEON_RED);
    enterView(View::ToolMenu);
  } else if (view == View::HistorySettings) {
    if (selection < 4) {
      const bool saved = ultimate_service.setHistoryCapacity(history_options[selection]);
      task->showAlert(saved ? "History setting saved" : "History change failed", 1000,
                      saved ? NEON_GREEN : NEON_RED);
    } else if (selection == 4) {
      if (!history_clear_armed) {
        history_clear_armed = true;
        task->showAlert("Double again to erase", 1000, NEON_RED);
      } else {
        const bool cleared = ultimate_service.clearHistory();
        history_clear_armed = false;
        task->showAlert(cleared ? "History erased" : "Erase failed", 1000,
                        cleared ? NEON_GREEN : NEON_RED);
      }
    } else enterView(View::ToolMenu);
  }
  task->requestRefresh();
  return true;
}

void UltimateUIScreen::poll() {
  if (shutdown_pending && !task->isButtonPressed()) task->shutdown();
  const uint32_t now = millis();
  if (draft_dirty && static_cast<int32_t>(now - draft_save_due) >= 0) persistDraft();
  if (view != View::Keyboard || task->isButtonPressed()) return;
  if (static_cast<int32_t>(now - next_scan) < 0) return;
  if (scan_phase == ScanPhase::Group) scan_group = (scan_group + 1) % keyboard_group_count;
  else {
    const uint8_t count = scan_group < 5 ? strlen(keyboard_groups[scan_group]) : 5;
    scan_item = (scan_item + 1) % count;
  }
  next_scan = now + ultimate_service.getSettings().scan_cadence_ms;
  task->requestRefresh();
}

void UltimateUIScreen::openInbox() {
  area = Area::Inbox;
  enterView(View::InboxThreads);
}

void UltimateUIScreen::handleTriplePress() {
  if (draft_dirty) persistDraft();
  shutdown_pending = false;
  history_clear_armed = false;
  if (area == Area::Inbox) {
    const bool cleared = ultimate_service.markAllRead();
    task->setLocalUnread(ultimate_service.getSnapshot().unread_count);
    task->showAlert(cleared ? "Unread cleared" : "Unread update failed", 1000,
                    cleared ? NEON_GREEN : NEON_RED);
    area = Area::Home;
    view = View::Root;
    startAreaTransition(-1);
  } else if (view != View::Root) {
    enterView(View::Root);
    return;
  } else if (area != Area::Home) {
    area = Area::Home;
    view = View::Root;
    selection = 0;
    startAreaTransition(-1);
  }
  task->requestRefresh();
}

void UltimateUIScreen::showPowerConfirm() {
  area = Area::Power;
  view = View::Root;
  shutdown_pending = false;
  startAreaTransition(1);
  task->requestRefresh();
}

void UltimateUIScreen::requestShutdown() {
  area = Area::Power;
  view = View::Root;
  shutdown_pending = true;
  task->requestRefresh();
}

#endif
