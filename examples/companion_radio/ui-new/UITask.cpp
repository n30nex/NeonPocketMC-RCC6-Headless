#include "UITask.h"
#include <helpers/TxtDataHelpers.h>
#include "../MyMesh.h"
#include "target.h"
#ifdef NEONPOCKET_UI
  #include "NeonPocketSplash.h"
#endif
#ifdef NEONPOCKET_ULTIMATE
  #include "UltimateUIScreen.h"
  #include "../UltimateService.h"
  #include <helpers/ui/NV3001BDisplay.h>
#endif
#ifdef RCC6_WEB_AP
  #include <helpers/esp32/SerialWebInterface.h>
  extern SerialWebInterface web_interface;
#endif
#ifdef WIFI_SSID
  #include <WiFi.h>
#endif

#ifndef AUTO_OFF_MILLIS
  #define AUTO_OFF_MILLIS     15000   // 15 seconds
#endif
#define BOOT_SCREEN_MILLIS   3000   // stock UI fallback

static uint32_t displayAutoOffMillis() {
#ifdef NEONPOCKET_ULTIMATE
  return ultimate_service.getDisplayTimeoutMillis();
#else
  return AUTO_OFF_MILLIS;
#endif
}

#ifdef NEONPOCKET_UI
#ifdef NEONPOCKET_ULTIMATE
  #define NEON_FRAME_MILLIS       66
#else
  #define NEON_FRAME_MILLIS       NeonPocketSplash::FRAME_MILLIS
#endif
  #define NEON_TRANSITION_MILLIS  300
  #define NEON_POWER_CONFIRM_MILLIS 8000
#endif

#ifdef NEONPOCKET_UI
static uint16_t neonFrameMillis() {
#ifdef NEONPOCKET_ULTIMATE
  return ultimate_service.getRecommendedFrameMillis();
#else
  return NEON_FRAME_MILLIS;
#endif
}
#endif

#ifdef PIN_STATUS_LED
#define LED_ON_MILLIS     20
#define LED_ON_MSG_MILLIS 200
#define LED_CYCLE_MILLIS  4000
#endif

#define LONG_PRESS_MILLIS   1200

#ifndef UI_RECENT_LIST_SIZE
  #define UI_RECENT_LIST_SIZE 4
#endif
// ponytail: fixed RAM ring; at capacity the UI reports an honest lower bound.
static constexpr int UI_MAX_UNREAD_MSGS = 32;

#if UI_HAS_JOYSTICK
  #define PRESS_LABEL "press Enter"
#else
  #define PRESS_LABEL "long press"
#endif

#include "icons.h"

#ifdef NEONPOCKET_UI
#ifdef NEONPOCKET_RCC6_UI_EXTENSIONS
static const char* const RCC6_QUICK_REPLIES[] = {
  "OK", "YES", "NO", "ON MY WAY", "NEED HELP", "73"
};
static constexpr uint8_t RCC6_QUICK_REPLY_COUNT =
    sizeof(RCC6_QUICK_REPLIES) / sizeof(RCC6_QUICK_REPLIES[0]);
static constexpr unsigned long RCC6_QUICK_REPLY_SCAN_MILLIS = 800;
#endif
#endif

class SplashScreen : public UIScreen {
  UITask* _task;
  unsigned long started_at;
  char _version_info[12];

public:
  SplashScreen(UITask* task) : _task(task) {
    // strip off dash and commit hash by changing dash to null terminator
    // e.g: v1.2.3-abcdef -> v1.2.3
#ifdef NEONPOCKET_UI
    NeonPocketSplash::shortVersion(_version_info, sizeof(_version_info), FIRMWARE_VERSION);
#else
    const char *ver = FIRMWARE_VERSION;
    const char *dash = strchr(ver, '-');
    int len = dash ? dash - ver : strlen(ver);
    if (len >= sizeof(_version_info)) len = sizeof(_version_info) - 1;
    memcpy(_version_info, ver, len);
    _version_info[len] = 0;
#endif
    started_at = millis();
  }

  int render(DisplayDriver& display) override {
#ifdef NEONPOCKET_UI
    const unsigned long elapsed = millis() - started_at;
    NeonPocketSplash::drawFrame(display, elapsed, _version_info, FIRMWARE_BUILD_DATE);
    return elapsed < NeonPocketSplash::DURATION_MILLIS ? NEON_FRAME_MILLIS : 500;
#else
    // meshcore logo
    display.setColor(NEON_BLUE);
    int logoWidth = 128;
    display.drawXbm((display.width() - logoWidth) / 2, 3, meshcore_logo, logoWidth, 13);

    // meshcore website
    const char* website = "https://meshcore.io";
    display.setColor(NEON_LIGHT);
    display.setTextSize(1);
    uint16_t websiteWidth = display.getTextWidth(website);
    display.setCursor((display.width() - websiteWidth) / 2, 22);
    display.print(website);

    // version info
    display.setColor(NEON_LIGHT);
    display.setTextSize(1);
    display.drawTextCentered(display.width()/2, 35, _version_info);

    display.setTextSize(1);
    display.drawTextCentered(display.width()/2, 48, FIRMWARE_BUILD_DATE);

    return 1000;
#endif
  }

  void poll() override {
    const unsigned long duration =
#ifdef NEONPOCKET_UI
        NeonPocketSplash::DURATION_MILLIS;
#else
        BOOT_SCREEN_MILLIS;
#endif
    if (millis() - started_at >= duration) {
      _task->gotoHomeScreen();
    }
  }
};

class HomeScreen : public UIScreen {
  enum HomePage {
    FIRST,
    RECENT,
    RADIO,
    BLUETOOTH,
    ADVERT,
#ifdef NEONPOCKET_RCC6_UI_EXTENSIONS
    QUICK_REPLY,
    DIAGNOSTICS,
#endif
#if ENV_INCLUDE_GPS == 1
    GPS,
#endif
#if UI_SENSORS_PAGE == 1
    SENSORS,
#endif
    SHUTDOWN,
    Count    // keep as last
  };

  UITask* _task;
  mesh::RTCClock* _rtc;
  SensorManager* _sensors;
  NodePrefs* _node_prefs;
  uint8_t _page;
  bool _shutdown_init;
  AdvertPath recent[UI_RECENT_LIST_SIZE];
#ifdef NEONPOCKET_UI
  int8_t _transition_dir = 0;
  unsigned long _transition_started = 0;
  bool _transition_pending = false;
#ifdef NEONPOCKET_RCC6_UI_EXTENSIONS
  uint8_t _quick_reply_index = 0;
  bool _quick_reply_confirm = false;
  bool _quick_reply_pressed = false;
  unsigned long _quick_reply_next = 0;

  void startQuickReplyScan() {
    _quick_reply_index = 0;
    _quick_reply_confirm = false;
    _quick_reply_pressed = false;
    _quick_reply_next = millis() + 1200;
    _task->requestRefresh();
  }
#endif
#endif


  void renderBatteryIndicator(DisplayDriver& display, uint16_t batteryMilliVolts) {
    // Convert millivolts to percentage
#ifndef BATT_MIN_MILLIVOLTS
  #define BATT_MIN_MILLIVOLTS 3000
#endif
#ifndef BATT_MAX_MILLIVOLTS
  #define BATT_MAX_MILLIVOLTS 4200
#endif
    const int minMilliVolts = BATT_MIN_MILLIVOLTS;
    const int maxMilliVolts = BATT_MAX_MILLIVOLTS;
    int batteryPercentage = ((batteryMilliVolts - minMilliVolts) * 100) / (maxMilliVolts - minMilliVolts);
    if (batteryPercentage < 0) batteryPercentage = 0; // Clamp to 0%
    if (batteryPercentage > 100) batteryPercentage = 100; // Clamp to 100%

    // battery icon
    int iconWidth = 24;
    int iconHeight = 10;
    int iconX = display.width() - iconWidth - 5; // Position the icon near the top-right corner
    int iconY = 0;
#ifdef NEONPOCKET_UI
    if (batteryMilliVolts == 0) {
      display.setColor(NEON_BLUE);
    } else if (batteryPercentage <= 15) {
      display.setColor(NEON_RED);
    } else if (batteryPercentage <= 40) {
      display.setColor(NEON_ORANGE);
    } else {
      display.setColor(NEON_GREEN);
    }
#else
    display.setColor(NEON_GREEN);
#endif

    // battery outline
    display.drawRect(iconX, iconY, iconWidth, iconHeight);

    // battery "cap"
    display.fillRect(iconX + iconWidth, iconY + (iconHeight / 4), 3, iconHeight / 2);

    if (batteryMilliVolts == 0) {
      display.setCursor(iconX + 6, iconY + 1);
      display.print("--");
      return;
    }

    // fill the battery based on the percentage
    int fillWidth = (batteryPercentage * (iconWidth - 4)) / 100;
    display.fillRect(iconX + 2, iconY + 2, fillWidth, iconHeight - 4);

    // show muted icon if buzzer is muted
#ifdef PIN_BUZZER
    if (_task->isBuzzerQuiet()) {
      display.setColor(NEON_RED);
      display.drawXbm(iconX - 9, iconY + 1, muted_icon, 8, 8);
    }
#endif
  }

  CayenneLPP sensors_lpp;
  int sensors_nb = 0;
  bool sensors_scroll = false;
  int sensors_scroll_offset = 0;
  int next_sensors_refresh = 0;

  void refresh_sensors() {
    if (millis() > next_sensors_refresh) {
      sensors_lpp.reset();
      sensors_nb = 0;
      sensors_lpp.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);
      sensors.querySensors(0xFF, sensors_lpp);
      LPPReader reader (sensors_lpp.getBuffer(), sensors_lpp.getSize());
      uint8_t channel, type;
      while(reader.readHeader(channel, type)) {
        reader.skipData(type);
        sensors_nb ++;
      }
      sensors_scroll = sensors_nb > UI_RECENT_LIST_SIZE;
#if AUTO_OFF_MILLIS > 0
      next_sensors_refresh = millis() + 5000; // refresh sensor values every 5 sec
#else
      next_sensors_refresh = millis() + 60000; // refresh sensor values every 1 min
#endif
    }
  }

#ifdef NEONPOCKET_UI
  const char* neonPageTitle() const {
    switch (_page) {
      case HomePage::FIRST: return "HOME";
      case HomePage::RECENT: return "NEARBY";
      case HomePage::RADIO: return "RADIO";
      case HomePage::BLUETOOTH:
#ifdef RCC6_WEB_AP
        return web_interface.isStationMode() ? "LOCAL WIFI" : "SETUP AP";
#else
        return "BLUETOOTH";
#endif
      case HomePage::ADVERT: return "ADVERTISE";
#ifdef NEONPOCKET_RCC6_UI_EXTENSIONS
      case HomePage::QUICK_REPLY: return "QUICK REPLY";
      case HomePage::DIAGNOSTICS: return "DIAGNOSTICS";
#endif
#if ENV_INCLUDE_GPS == 1
      case HomePage::GPS: return "GPS";
#endif
#if UI_SENSORS_PAGE == 1
      case HomePage::SENSORS: return "SENSORS";
#endif
      case HomePage::SHUTDOWN: return "POWER";
      default: return "MESHCORE";
    }
  }

  const char* neonActionHint() const {
    if (_page == HomePage::SHUTDOWN) {
      return _task->isPowerConfirmArmed() ? "HOLD AGAIN TO CONFIRM" : "HOLD FOR POWER";
    }
    if (_page == HomePage::FIRST && _task->getMsgCount() > 0) return "CLICK NEXT  2X INBOX";
    if (_page == HomePage::BLUETOOTH) {
#ifdef RCC6_WEB_AP
      return web_interface.isStationMode() ? "CLICK NEXT  2X SETUP AP" : "CLICK NEXT  2X TOGGLE";
#else
      return "CLICK NEXT  2X TOGGLE";
#endif
    }
    if (_page == HomePage::ADVERT) return "CLICK NEXT  2X SEND";
#ifdef NEONPOCKET_RCC6_UI_EXTENSIONS
    if (_page == HomePage::QUICK_REPLY) {
      return _quick_reply_confirm ? "2X CONFIRM" : "2X SELECT";
    }
#endif
    return "CLICK NEXT";
  }

  void renderNeonHeader(DisplayDriver& display) {
    display.setTextSize(1);
    display.setColor(NEON_GREEN);
    char filtered_name[sizeof(_node_prefs->node_name)];
    display.translateUTF8ToBlocks(filtered_name, _node_prefs->node_name, sizeof(filtered_name));
    display.drawTextEllipsized(4, 1, 130, filtered_name);

    ColorVal link_color = NEON_RED;
#ifdef RCC6_WEB_AP
    if (_task->isSerialEnabled()) {
      link_color = _task->hasConnection() ? NEON_GREEN : NEON_ORANGE;
    }
#else
    if (_task->isBluetoothEnabled()) {
      link_color = _task->hasConnection() ? NEON_GREEN : NEON_ORANGE;
    }
#endif
    display.setColor(link_color);
    display.fillRect(140, 5, 6, 6);
    display.setColor(NEON_LIGHT);
    display.setCursor(151, 1);
#ifdef RCC6_WEB_AP
    display.print("WIFI");
#else
    display.print("BLE");
#endif
    renderBatteryIndicator(display, _task->getCachedBattMilliVolts());

    display.setColor(NEON_BLUE);
    display.fillRect(0, 17, display.width(), 2);
    if (_page == HomePage::FIRST) {
      display.setColor(NEON_GREEN);
      display.setCursor(4, 21);
      display.print("HOME");
      char radio_health[48];
      if (_task->hasCachedRadioSample()) {
        snprintf(radio_health, sizeof(radio_health), "RF %ddBm %.1fdB",
            (int)_task->getCachedRadioRSSI(), _task->getCachedRadioSNRQuarter() / 4.0f);
      } else {
        snprintf(radio_health, sizeof(radio_health), "%.3f SF%u", _node_prefs->freq,
            _node_prefs->sf);
      }
      display.setColor(NEON_BLUE);
      display.drawTextRightAlign(display.width() - 4, 21, radio_health);
    } else {
      display.setColor(_page == HomePage::SHUTDOWN ? NEON_RED : NEON_LIGHT);
      display.drawTextCentered(display.width() / 2, 21, neonPageTitle());
    }
  }

  void renderNeonFooter(DisplayDriver& display) {
    int x = 7;
    const int y = display.height() - 7;
    for (uint8_t i = 0; i < HomePage::Count; i++, x += 10) {
      if (i == _page) {
        display.setColor(NEON_GREEN);
        display.fillRect(x - 2, y - 2, 5, 5);
      } else {
        display.setColor(NEON_BLUE);
        display.fillRect(x, y, 2, 2);
      }
    }

    const char* hint = neonActionHint();
    if (hint[0]) {
      display.setTextSize(1);
      display.setColor(_page == HomePage::SHUTDOWN ? NEON_RED : NEON_BLUE);
      display.drawTextRightAlign(display.width() - 4, 113, hint);
    }
  }

  bool renderNeonTransition(DisplayDriver& display) {
    if (_transition_dir == 0) return false;
    if (_transition_pending) {
      _transition_pending = false;
      return true;
    }
    if (_transition_started == 0) _transition_started = millis();

    const unsigned long elapsed = millis() - _transition_started;
    if (elapsed >= NEON_TRANSITION_MILLIS) {
      _transition_dir = 0;
      _transition_started = 0;
      return false;
    }

    const int travel = 72 * elapsed / NEON_TRANSITION_MILLIS;
    const int line_y = _transition_dir > 0 ? 37 + travel : 109 - travel;
    display.setColor(NEON_BLUE);
    display.fillRect(12, line_y, display.width() - 24, 2);
    return true;
  }

  int renderNeon(DisplayDriver& display) {
    char tmp[80];
    renderNeonHeader(display);

    if (_page == HomePage::FIRST) {
      display.setColor(NEON_BLUE);
      display.drawRect(4, 38, display.width() - 8, 40);
      display.setTextSize(1);
      snprintf(tmp, sizeof(tmp), "%d%s UNREAD", _task->getMsgCount(),
          _task->hasUnreadOverflow() ? "+" : "");
      display.setColor(_task->getMsgCount() ? NEON_YELLOW : NEON_GREEN);
      display.setCursor(8, 42);
      display.print(tmp);
      if (_task->hasLatestPreview()) {
        char filtered_sender[32];
        display.translateUTF8ToBlocks(filtered_sender, _task->getLatestSender(), sizeof(filtered_sender));
        display.setColor(_task->getLatestSender()[0] == '#' ? NEON_BLUE : NEON_GREEN);
        display.drawTextEllipsized(88, 42, display.width() - 96, filtered_sender);
        char filtered_preview[48];
        display.translateUTF8ToBlocks(filtered_preview, _task->getLatestPreview(), sizeof(filtered_preview));
        display.setColor(NEON_LIGHT);
        display.drawTextEllipsized(8, 59, display.width() - 16, filtered_preview);
      } else {
        display.setColor(NEON_LIGHT);
        display.drawTextCentered(display.width() / 2, 59,
            _task->getMsgCount() ? "Open inbox to read" : "No unread messages");
      }

      the_mesh.getRecentlyHeard(recent, UI_RECENT_LIST_SIZE);
      AdvertPath* latest = nullptr;
      for (int i = 0; i < UI_RECENT_LIST_SIZE; i++) {
        if (recent[i].name[0]) {
          latest = &recent[i];
          break;
        }
      }
      display.setColor(NEON_BLUE);
      display.drawRect(4, 81, display.width() - 8, 29);
      display.setTextSize(1);
      display.setColor(NEON_BLUE);
      display.setCursor(8, 86);
      display.print("NEARBY");
      if (latest) {
        int secs = _rtc->getCurrentTime() - latest->recv_timestamp;
        if (secs < 0) secs = 0;
        if (secs < 60) snprintf(tmp, sizeof(tmp), "%ds", secs);
        else if (secs < 3600) snprintf(tmp, sizeof(tmp), "%dm", secs / 60);
        else snprintf(tmp, sizeof(tmp), "%dh", secs / 3600);
        char filtered_recent_name[sizeof(latest->name)];
        display.translateUTF8ToBlocks(filtered_recent_name, latest->name, sizeof(filtered_recent_name));
        display.setColor(NEON_GREEN);
        display.drawTextEllipsized(58, 86, 126, filtered_recent_name);
        display.setColor(NEON_LIGHT);
        display.drawTextRightAlign(display.width() - 8, 86, tmp);
      } else {
        display.setColor(NEON_LIGHT);
        display.drawTextRightAlign(display.width() - 8, 86, "Listening...");
      }
    } else if (_page == HomePage::RECENT) {
      the_mesh.getRecentlyHeard(recent, UI_RECENT_LIST_SIZE);
      display.setTextSize(1);
      bool any = false;
      int y = 39;
      for (int i = 0; i < UI_RECENT_LIST_SIZE; i++, y += 18) {
        auto a = &recent[i];
        if (a->name[0] == 0) continue;
        any = true;
        int secs = _rtc->getCurrentTime() - a->recv_timestamp;
        if (secs < 0) secs = 0;
        if (secs < 60) {
          snprintf(tmp, sizeof(tmp), "%ds", secs);
        } else if (secs < 3600) {
          snprintf(tmp, sizeof(tmp), "%dm", secs / 60);
        } else {
          snprintf(tmp, sizeof(tmp), "%dh", secs / 3600);
        }
        char filtered_recent_name[sizeof(a->name)];
        display.translateUTF8ToBlocks(filtered_recent_name, a->name, sizeof(filtered_recent_name));
        display.setColor(NEON_GREEN);
        display.fillRect(5, y + 5, 5, 5);
        display.drawTextEllipsized(16, y, 166, filtered_recent_name);
        display.setColor(NEON_LIGHT);
        display.drawTextRightAlign(display.width() - 4, y, tmp);
        display.setColor(NEON_BLUE);
        display.fillRect(16, y + 15, display.width() - 20, 1);
      }
      if (!any) {
        display.setColor(NEON_BLUE);
        display.drawRect(20, 49, display.width() - 40, 42);
        display.setColor(NEON_LIGHT);
        display.drawTextCentered(display.width() / 2, 63, "Listening for adverts");
      }
    } else if (_page == HomePage::RADIO) {
      display.setColor(NEON_BLUE);
      display.drawRect(4, 38, 103, 44);
      display.drawRect(113, 38, 103, 44);
      display.setTextSize(1);
      display.setColor(NEON_LIGHT);
      display.drawTextCentered(55, 42, "RSSI dBm");
      display.drawTextCentered(165, 42, "SNR dB");
      display.setTextSize(2);
      display.setColor(NEON_GREEN);
      if (_task->hasCachedRadioSample()) {
        snprintf(tmp, sizeof(tmp), "%d", (int)_task->getCachedRadioRSSI());
      } else {
        strcpy(tmp, "--");
      }
      display.drawTextCentered(55, 58, tmp);
      if (_task->hasCachedRadioSample()) {
        snprintf(tmp, sizeof(tmp), "%.1f", _task->getCachedRadioSNRQuarter() / 4.0f);
      } else {
        strcpy(tmp, "--");
      }
      display.drawTextCentered(165, 58, tmp);

      display.setTextSize(1);
      display.setColor(NEON_LIGHT);
      snprintf(tmp, sizeof(tmp), "%.3fMHz BW%.1f SF%u CR%u TX%d", _node_prefs->freq,
          _node_prefs->bw, _node_prefs->sf, _node_prefs->cr, _node_prefs->tx_power_dbm);
      display.drawTextEllipsized(4, 84, display.width() - 8, tmp);
      display.setColor(radio_driver.getPacketsRecvErrors() ? NEON_RED : NEON_GREEN);
      snprintf(tmp, sizeof(tmp), "RX%lu TX%lu ERR%lu NF%d",
          (unsigned long)radio_driver.getPacketsRecv(),
          (unsigned long)radio_driver.getPacketsSent(),
          (unsigned long)radio_driver.getPacketsRecvErrors(), radio_driver.getNoiseFloor());
      display.drawTextEllipsized(4, 99, display.width() - 8, tmp);
    } else if (_page == HomePage::BLUETOOTH) {
#ifdef RCC6_WEB_AP
      const bool enabled = _task->isSerialEnabled();
      const bool station = web_interface.isStationMode();
      display.setTextSize(1);
      display.setColor(enabled ? NEON_GREEN : NEON_RED);
      display.setCursor(8, 40);
      display.print(enabled ? (station ? "LOCAL WIFI ON" : "SETUP AP ON") : "NETWORK OFF");
      display.setColor(_task->hasConnection() ? NEON_GREEN : NEON_ORANGE);
      display.drawTextRightAlign(display.width() - 8, 40,
          _task->hasConnection() ? "CLIENT READY" : "WAITING");

      display.setColor(NEON_BLUE);
      display.drawRect(4, 54, display.width() - 8, 55);
      display.setColor(NEON_LIGHT);
      snprintf(tmp, sizeof(tmp), "SSID %s", web_interface.getCurrentSsid());
      display.drawTextEllipsized(8, 59, display.width() - 16, tmp);
      if (station) {
        snprintf(tmp, sizeof(tmp), "AUTH meshcore/%s", web_interface.getApPassword());
      } else {
        snprintf(tmp, sizeof(tmp), "PASS %s%s", web_interface.getApPassword(),
            web_interface.isFallbackActive() ? "  FALLBACK" : "");
      }
      display.drawTextEllipsized(8, 75, display.width() - 16, tmp);
      const IPAddress current_ip = web_interface.getCurrentIP();
      snprintf(tmp, sizeof(tmp), "OPEN %u.%u.%u.%u", current_ip[0], current_ip[1],
          current_ip[2], current_ip[3]);
      display.drawTextEllipsized(8, 91, display.width() - 16, tmp);
#else
      const bool enabled = _task->isBluetoothEnabled();
      display.setColor(enabled ? NEON_GREEN : NEON_RED);
      display.drawXbm(24, 50, enabled ? bluetooth_on : bluetooth_off, 32, 32);
      display.setTextSize(2);
      display.drawTextCentered(146, 40, enabled ? "BLE ON" : "BLE OFF");
      display.setTextSize(1);
      display.setColor(NEON_LIGHT);
      display.drawTextCentered(146, 68,
          _task->hasConnection() ? "Phone connected" : "Waiting for phone");
      if (enabled && !_task->hasConnection()) {
        snprintf(tmp, sizeof(tmp), "PIN %06lu", (unsigned long)the_mesh.getBLEPin());
        display.setColor(NEON_ORANGE);
        display.drawTextCentered(146, 86, tmp);
      } else {
        display.setColor(NEON_BLUE);
        display.drawTextCentered(146, 86,
            _task->hasConnection() ? "Companion ready" : "Radio only");
      }
#endif
    } else if (_page == HomePage::ADVERT) {
      display.setColor(NEON_GREEN);
      display.drawXbm(24, 50, advert_icon, 32, 32);
      display.setTextSize(2);
      display.setColor(NEON_YELLOW);
      display.drawTextCentered(146, 40, "ADVERTISE");
      display.setTextSize(1);
      display.setColor(NEON_LIGHT);
      display.drawTextCentered(146, 70, "Announce this node");
      display.setColor(NEON_BLUE);
      display.drawTextCentered(146, 90, "Double to send");
#ifdef NEONPOCKET_RCC6_UI_EXTENSIONS
    } else if (_page == HomePage::QUICK_REPLY) {
      display.setTextSize(1);
      if (_quick_reply_confirm) {
        display.setColor(NEON_YELLOW);
        display.drawRect(8, 40, display.width() - 16, 48);
        display.drawTextCentered(display.width() / 2, 46, "CONFIRM QUICK REPLY");
        display.setTextSize(2);
        display.setColor(NEON_LIGHT);
        display.drawTextCentered(display.width() / 2, 62,
            RCC6_QUICK_REPLIES[_quick_reply_index]);
        display.setTextSize(1);
        display.setColor(NEON_ORANGE);
        display.drawTextCentered(display.width() / 2, 94, "DOUBLE-PRESS TO SEND");
      } else {
        for (uint8_t i = 0; i < RCC6_QUICK_REPLY_COUNT; i++) {
          const int x = 4 + (i % 2) * 108;
          const int y = 38 + (i / 2) * 23;
          const bool selected = i == _quick_reply_index;
          display.setColor(selected ? NEON_YELLOW : NEON_BLUE);
          display.drawRect(x, y, 104, 20);
          display.setColor(selected ? NEON_YELLOW : NEON_LIGHT);
          display.drawTextCentered(x + 52, y + 6, RCC6_QUICK_REPLIES[i]);
        }
      }
    } else if (_page == HomePage::DIAGNOSTICS) {
      const uint32_t uptime = _task->getCachedUptimeSeconds();
      display.setTextSize(1);
      display.setColor(NEON_BLUE);
      display.drawRect(4, 38, display.width() - 8, 32);
      display.drawRect(4, 75, display.width() - 8, 34);
      display.setColor(NEON_LIGHT);
      if (_task->getCachedBattMilliVolts()) {
        snprintf(tmp, sizeof(tmp), "UP %luh%02lum   BAT %umV",
            (unsigned long)(uptime / 3600), (unsigned long)((uptime / 60) % 60),
            (unsigned)_task->getCachedBattMilliVolts());
      } else {
        snprintf(tmp, sizeof(tmp), "UP %luh%02lum   BAT --",
            (unsigned long)(uptime / 3600), (unsigned long)((uptime / 60) % 60));
      }
      display.setCursor(9, 43);
      display.print(tmp);
      snprintf(tmp, sizeof(tmp), "HEAP %luK   MAX %luK",
          (unsigned long)(_task->getCachedHeapFree() / 1024),
          (unsigned long)(_task->getCachedHeapMax() / 1024));
      display.setCursor(9, 57);
      display.print(tmp);
      snprintf(tmp, sizeof(tmp), "RX %lu   TX %lu",
          (unsigned long)_task->getCachedRxPackets(),
          (unsigned long)_task->getCachedTxPackets());
      display.setCursor(9, 81);
      display.print(tmp);
      display.setColor(_task->getCachedRxErrors() ? NEON_RED : NEON_GREEN);
      snprintf(tmp, sizeof(tmp), "ERR %lu   NOISE %ddBm",
          (unsigned long)_task->getCachedRxErrors(), (int)_task->getCachedNoiseFloor());
      display.setCursor(9, 95);
      display.print(tmp);
#endif
#if ENV_INCLUDE_GPS == 1
    } else if (_page == HomePage::GPS) {
      LocationProvider* nmea = _sensors->getLocationProvider();
      const bool gps_on = _task->getGPSState();
      display.setTextSize(2);
      display.setColor(gps_on ? NEON_GREEN : NEON_RED);
      display.drawTextCentered(display.width() / 2, 45, gps_on ? "GPS ON" : "GPS OFF");
      display.setTextSize(1);
      display.setColor(NEON_LIGHT);
      if (!nmea) {
        display.drawTextCentered(display.width() / 2, 76, "GPS unavailable");
      } else {
        snprintf(tmp, sizeof(tmp), "%s | %d satellites", nmea->isValid() ? "FIX" : "NO FIX",
            nmea->satellitesCount());
        display.drawTextCentered(display.width() / 2, 76, tmp);
      }
#endif
#if UI_SENSORS_PAGE == 1
    } else if (_page == HomePage::SENSORS) {
      refresh_sensors();
      display.setTextSize(2);
      display.setColor(NEON_GREEN);
      snprintf(tmp, sizeof(tmp), "%d", sensors_nb);
      display.drawTextCentered(display.width() / 2, 45, tmp);
      display.setTextSize(1);
      display.setColor(NEON_LIGHT);
      display.drawTextCentered(display.width() / 2, 76, "sensor values available");
#endif
    } else if (_page == HomePage::SHUTDOWN) {
      if (_shutdown_init) {
        display.setTextSize(2);
        display.setColor(NEON_RED);
        display.drawTextCentered(display.width() / 2, 55, "HIBERNATING");
      } else {
        display.setColor(NEON_RED);
        display.drawXbm(24, 50, power_icon, 32, 32);
        display.setTextSize(2);
        display.drawTextCentered(146, 40, "POWER OFF");
        display.setTextSize(1);
        display.setColor(NEON_LIGHT);
        display.drawTextCentered(146, 72,
            _task->isPowerConfirmArmed() ? "Hold again" : "Hold button");
        display.setColor(NEON_RED);
        display.drawTextCentered(146, 91, "to power off");
      }
    }

    const bool transitioning = renderNeonTransition(display);
    renderNeonFooter(display);
    if (transitioning) return NEON_FRAME_MILLIS;
    return _page == HomePage::RADIO ? 1000 : 3000;
  }
#endif

public:
#ifdef NEONPOCKET_UI
  void neonGoHome() {
    _page = HomePage::FIRST;
    _shutdown_init = false;
    _transition_dir = -1;
    _transition_started = 0;
    _transition_pending = true;
#ifdef NEONPOCKET_RCC6_UI_EXTENSIONS
    _quick_reply_confirm = false;
#endif
  }

  void neonRequestShutdown() {
    _page = HomePage::SHUTDOWN;
    _shutdown_init = true;
  }

  void neonShowPowerConfirm() {
    _page = HomePage::SHUTDOWN;
    _shutdown_init = false;
    _transition_dir = 1;
    _transition_started = 0;
    _transition_pending = true;
  }

  bool neonIsPowerPage() const { return _page == HomePage::SHUTDOWN; }
#endif

  HomeScreen(UITask* task, mesh::RTCClock* rtc, SensorManager* sensors, NodePrefs* node_prefs)
     : _task(task), _rtc(rtc), _sensors(sensors), _node_prefs(node_prefs), _page(0),
       _shutdown_init(false), sensors_lpp(200) {  }

  void poll() override {
    if (_shutdown_init && !_task->isButtonPressed()) {  // must wait for USR button to be released
      _task->shutdown();
    }
#ifdef NEONPOCKET_RCC6_UI_EXTENSIONS
    if (_page != HomePage::QUICK_REPLY || _quick_reply_confirm) return;

    const unsigned long now = millis();
    if (_task->isButtonPressed()) {
      _quick_reply_pressed = true;
      return;
    }
    if (_quick_reply_pressed) {
      _quick_reply_pressed = false;
      _quick_reply_next = now + RCC6_QUICK_REPLY_SCAN_MILLIS;
      return;
    }
    if ((int32_t)(now - _quick_reply_next) >= 0) {
      _quick_reply_index = (_quick_reply_index + 1) % RCC6_QUICK_REPLY_COUNT;
      _quick_reply_next = now + RCC6_QUICK_REPLY_SCAN_MILLIS;
      _task->requestRefresh();
    }
#endif
  }

  int render(DisplayDriver& display) override {
#ifdef NEONPOCKET_UI
    return renderNeon(display);
#else
    char tmp[80];
    // node name
    display.setTextSize(1);
    display.setColor(NEON_GREEN);
    char filtered_name[sizeof(_node_prefs->node_name)];
    display.translateUTF8ToBlocks(filtered_name, _node_prefs->node_name, sizeof(filtered_name));
    display.setCursor(0, 0);
    display.print(filtered_name);

    // battery voltage
    renderBatteryIndicator(display, _task->getBattMilliVolts());

    // curr page indicator
    int y = 14;
    int x = display.width() / 2 - 5 * (HomePage::Count-1);
    for (uint8_t i = 0; i < HomePage::Count; i++, x += 10) {
      if (i == _page) {
        display.fillRect(x-1, y-1, 3, 3);
      } else {
        display.fillRect(x, y, 1, 1);
      }
    }

    if (_page == HomePage::FIRST) {
      display.setColor(NEON_YELLOW);
      display.setTextSize(2);
      sprintf(tmp, "MSG: %d", _task->getMsgCount());
      display.drawTextCentered(display.width() / 2, 20, tmp);

      #ifdef WIFI_SSID
        IPAddress ip = WiFi.localIP();
        snprintf(tmp, sizeof(tmp), "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        display.setTextSize(1);
        display.drawTextCentered(display.width() / 2, 54, tmp);
      #endif
      if (_task->hasConnection()) {
        display.setColor(NEON_GREEN);
        display.setTextSize(1);
        display.drawTextCentered(display.width() / 2, 43, "< Connected >");

      } else if (the_mesh.getBLEPin() != 0) { // BT pin
        display.setColor(NEON_RED);
        display.setTextSize(2);
        sprintf(tmp, "Pin:%d", the_mesh.getBLEPin());
        display.drawTextCentered(display.width() / 2, 43, tmp);
      }
    } else if (_page == HomePage::RECENT) {
      the_mesh.getRecentlyHeard(recent, UI_RECENT_LIST_SIZE);
      display.setColor(NEON_GREEN);
      int y = 20;
      for (int i = 0; i < UI_RECENT_LIST_SIZE; i++, y += 11) {
        auto a = &recent[i];
        if (a->name[0] == 0) continue;  // empty slot
        int secs = _rtc->getCurrentTime() - a->recv_timestamp;
        if (secs < 60) {
          sprintf(tmp, "%ds", secs);
        } else if (secs < 60*60) {
          sprintf(tmp, "%dm", secs / 60);
        } else {
          sprintf(tmp, "%dh", secs / (60*60));
        }

        int timestamp_width = display.getTextWidth(tmp);
        int max_name_width = display.width() - timestamp_width - 1;

        char filtered_recent_name[sizeof(a->name)];
        display.translateUTF8ToBlocks(filtered_recent_name, a->name, sizeof(filtered_recent_name));
        display.drawTextEllipsized(0, y, max_name_width, filtered_recent_name);
        display.setCursor(display.width() - timestamp_width - 1, y);
        display.print(tmp);
      }
    } else if (_page == HomePage::RADIO) {
      display.setColor(NEON_YELLOW);
      display.setTextSize(1);
      // freq / sf
      display.setCursor(0, 20);
      sprintf(tmp, "FQ: %06.3f   SF: %d", _node_prefs->freq, _node_prefs->sf);
      display.print(tmp);

      display.setCursor(0, 31);
      sprintf(tmp, "BW: %03.2f     CR: %d", _node_prefs->bw, _node_prefs->cr);
      display.print(tmp);

      // tx power,  noise floor
      display.setCursor(0, 42);
      sprintf(tmp, "TX: %ddBm", _node_prefs->tx_power_dbm);
      display.print(tmp);
      display.setCursor(0, 53);
      sprintf(tmp, "Noise floor: %d", radio_driver.getNoiseFloor());
      display.print(tmp);
    } else if (_page == HomePage::BLUETOOTH) {
      display.setColor(NEON_GREEN);
      display.drawXbm((display.width() - 32) / 2, 18,
          _task->isBluetoothEnabled() ? bluetooth_on : bluetooth_off,
          32, 32);
      display.setTextSize(1);
      display.drawTextCentered(display.width() / 2, 64 - 11, "toggle: " PRESS_LABEL);
    } else if (_page == HomePage::ADVERT) {
      display.setColor(NEON_GREEN);
      display.drawXbm((display.width() - 32) / 2, 18, advert_icon, 32, 32);
      display.drawTextCentered(display.width() / 2, 64 - 11, "advert: " PRESS_LABEL);
#if ENV_INCLUDE_GPS == 1
    } else if (_page == HomePage::GPS) {
      LocationProvider* nmea = sensors.getLocationProvider();
      char buf[50];
      int y = 18;
      bool gps_state = _task->getGPSState();
#ifdef PIN_GPS_SWITCH
      bool hw_gps_state = digitalRead(PIN_GPS_SWITCH);
      if (gps_state != hw_gps_state) {
        strcpy(buf, gps_state ? "gps off(hw)" : "gps off(sw)");
      } else {
        strcpy(buf, gps_state ? "gps on" : "gps off");
      }
#else
      strcpy(buf, gps_state ? "gps on" : "gps off");
#endif
      display.drawTextLeftAlign(0, y, buf);
      if (nmea == NULL) {
        y = y + 12;
        display.drawTextLeftAlign(0, y, "Can't access GPS");
      } else {
        strcpy(buf, nmea->isValid()?"fix":"no fix");
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
        display.drawTextLeftAlign(0, y, "sat");
        sprintf(buf, "%d", nmea->satellitesCount());
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
        display.drawTextLeftAlign(0, y, "pos");
        sprintf(buf, "%.4f %.4f",
          nmea->getLatitude()/1000000., nmea->getLongitude()/1000000.);
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
        display.drawTextLeftAlign(0, y, "alt");
        sprintf(buf, "%.2f", nmea->getAltitude()/1000.);
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
      }
#endif
#if UI_SENSORS_PAGE == 1
    } else if (_page == HomePage::SENSORS) {
      int y = 18;
      refresh_sensors();
      char buf[30];
      char name[30];
      LPPReader r(sensors_lpp.getBuffer(), sensors_lpp.getSize());

      for (int i = 0; i < sensors_scroll_offset; i++) {
        uint8_t channel, type;
        r.readHeader(channel, type);
        r.skipData(type);
      }

      for (int i = 0; i < (sensors_scroll?UI_RECENT_LIST_SIZE:sensors_nb); i++) {
        uint8_t channel, type;
        if (!r.readHeader(channel, type)) { // reached end, reset
          r.reset();
          r.readHeader(channel, type);
        }

        display.setCursor(0, y);
        float v;
        switch (type) {
          case LPP_GPS: // GPS
            float lat, lon, alt;
            r.readGPS(lat, lon, alt);
            strcpy(name, "gps"); sprintf(buf, "%.4f %.4f", lat, lon);
            break;
          case LPP_VOLTAGE:
            r.readVoltage(v);
            strcpy(name, "voltage"); sprintf(buf, "%6.2f", v);
            break;
          case LPP_CURRENT:
            r.readCurrent(v);
            strcpy(name, "current"); sprintf(buf, "%.3f", v);
            break;
          case LPP_TEMPERATURE:
            r.readTemperature(v);
            strcpy(name, "temperature"); sprintf(buf, "%.2f", v);
            break;
          case LPP_RELATIVE_HUMIDITY:
            r.readRelativeHumidity(v);
            strcpy(name, "humidity"); sprintf(buf, "%.2f", v);
            break;
          case LPP_BAROMETRIC_PRESSURE:
            r.readPressure(v);
            strcpy(name, "pressure"); sprintf(buf, "%.2f", v);
            break;
          case LPP_ALTITUDE:
            r.readAltitude(v);
            strcpy(name, "altitude"); sprintf(buf, "%.0f", v);
            break;
          case LPP_POWER:
            r.readPower(v);
            strcpy(name, "power"); sprintf(buf, "%6.2f", v);
            break;
          default:
            r.skipData(type);
            strcpy(name, "unk"); sprintf(buf, "");
        }
        display.setCursor(0, y);
        display.print(name);
        display.setCursor(
          display.width()-display.getTextWidth(buf)-1, y
        );
        display.print(buf);
        y = y + 12;
      }
      if (sensors_scroll) sensors_scroll_offset = (sensors_scroll_offset+1)%sensors_nb;
      else sensors_scroll_offset = 0;
#endif
    } else if (_page == HomePage::SHUTDOWN) {
      display.setColor(NEON_GREEN);
      display.setTextSize(1);
      if (_shutdown_init) {
        display.drawTextCentered(display.width() / 2, 34, "hibernating...");
      } else {
        display.drawXbm((display.width() - 32) / 2, 18, power_icon, 32, 32);
        display.drawTextCentered(display.width() / 2, 64 - 11, "hibernate:" PRESS_LABEL);
      }
    }
    return 5000;   // next render after 5000 ms
#endif
  }

  bool handleInput(char c) override {
    if (c == KEY_LEFT || c == KEY_PREV) {
      _page = (_page + HomePage::Count - 1) % HomePage::Count;
#ifdef NEONPOCKET_UI
      _transition_dir = -1;
      _transition_started = 0;
      _transition_pending = true;
#ifdef NEONPOCKET_RCC6_UI_EXTENSIONS
      if (_page == HomePage::QUICK_REPLY) startQuickReplyScan();
      else _quick_reply_confirm = false;
#endif
#endif
      return true;
    }
    if (c == KEY_NEXT || c == KEY_RIGHT) {
      _page = (_page + 1) % HomePage::Count;
#ifdef NEONPOCKET_UI
      _transition_dir = 1;
      _transition_started = 0;
      _transition_pending = true;
#ifdef NEONPOCKET_RCC6_UI_EXTENSIONS
      if (_page == HomePage::QUICK_REPLY) startQuickReplyScan();
      else _quick_reply_confirm = false;
#endif
#endif
      if (_page == HomePage::RECENT) {
#ifndef NEONPOCKET_UI
        _task->showAlert("Recent adverts", 800);
#endif
      }
      return true;
    }
#ifdef NEONPOCKET_UI
    if (c == KEY_ENTER && _page == HomePage::FIRST && _task->getMsgCount() > 0) {
      _task->gotoMsgPreviewScreen();
      return true;
    }
#endif
#ifdef NEONPOCKET_RCC6_UI_EXTENSIONS
    if (c == KEY_ENTER && _page == HomePage::QUICK_REPLY) {
      if (!_quick_reply_confirm) {
        _quick_reply_confirm = true;
        _task->requestRefresh();
      } else {
        const bool sent = the_mesh.sendQuickReplyToLatest(
            RCC6_QUICK_REPLIES[_quick_reply_index]);
        _task->showAlert(sent ? "Quick Reply sent" : "Quick Reply failed", 1200,
                         sent ? NEON_GREEN : NEON_RED);
        startQuickReplyScan();
      }
      return true;
    }
#endif
    if (c == KEY_ENTER && _page == HomePage::BLUETOOTH) {
#ifdef RCC6_WEB_AP
      if (web_interface.isStationMode()) {
        const bool saved = web_interface.selectSetupAp();
        _task->showAlert(saved ? "Restarting setup AP" : "Setup AP save failed", 1000,
            saved ? NEON_ORANGE : NEON_RED);
        return true;
      }
      if (_task->isSerialEnabled()) {
        _task->disableSerial();
      } else {
        _task->enableSerial();
      }
      _task->showAlert(_task->isSerialEnabled() ? "Setup AP on" : "Setup AP off", 800,
          _task->isSerialEnabled() ? NEON_GREEN : NEON_ORANGE);
#else
      if (_task->isBluetoothEnabled()) {  // toggle Bluetooth on/off
        _task->disableBluetooth();
      } else {
        _task->enableBluetooth();
      }
#ifdef NEONPOCKET_UI
      _task->showAlert(_task->isBluetoothEnabled() ? "Bluetooth on" : "Bluetooth off", 800,
          _task->isBluetoothEnabled() ? NEON_GREEN : NEON_ORANGE);
#endif
#endif
      return true;
    }
    if (c == KEY_ENTER && _page == HomePage::ADVERT) {
#ifdef NEONPOCKET_UI
      const bool advert_queued = the_mesh.advert(true);
#else
      _task->notify(UIEventType::ack);
      const bool advert_queued = the_mesh.advert();
#endif
      if (advert_queued) {
#ifdef NEONPOCKET_UI
        _task->armManualAdvert();
        _task->showAlert("Mesh advert queued", 1000, NEON_YELLOW);
#else
        _task->showAlert("Advert sent!", 1000);
#endif
      } else {
#ifdef NEONPOCKET_UI
        const bool busy = the_mesh.isAdvertPending();
        _task->showAlert(busy ? "Advert already queued" : "Advert failed", 1000,
            busy ? NEON_ORANGE : NEON_RED);
#else
        _task->showAlert("Advert failed..", 1000);
#endif
      }
      return true;
    }
#if ENV_INCLUDE_GPS == 1
    if (c == KEY_ENTER && _page == HomePage::GPS) {
      _task->toggleGPS();
      return true;
    }
#endif
#if UI_SENSORS_PAGE == 1
    if (c == KEY_ENTER && _page == HomePage::SENSORS) {
      _task->toggleGPS();
      next_sensors_refresh=0;
      return true;
    }
#endif
    if (c == KEY_ENTER && _page == HomePage::SHUTDOWN) {
#ifdef NEONPOCKET_UI
      _task->showAlert(_task->isPowerConfirmArmed() ? "Hold again to confirm" : "Hold to power off",
          900, NEON_RED);
#else
      _shutdown_init = true;  // need to wait for button to be released
#endif
      return true;
    }
    return false;
  }
};

class MsgPreviewScreen : public UIScreen {
  UITask* _task;
  mesh::RTCClock* _rtc;

  struct MsgEntry {
    uint32_t timestamp;
    uint8_t path_len;
    char source[32];
#ifdef NEONPOCKET_UI
    char msg[MAX_FRAME_SIZE];
#else
    char msg[78];
#endif
  };
  int num_unread;
  int head = UI_MAX_UNREAD_MSGS - 1; // index of latest unread message
  MsgEntry unread[UI_MAX_UNREAD_MSGS];
#ifdef NEONPOCKET_UI
  bool overflowed = false;
  int page_head = -1;
  size_t page_offset = 0;
  size_t next_page_offset = 0;
  bool page_has_more = false;

  size_t renderMessagePage(DisplayDriver& display, const char* text, size_t offset) {
    static constexpr size_t chars_per_line = 33;
    static constexpr int first_line_y = 48;
    static constexpr int line_height = 15;
    static constexpr int glyph_height = 14;
    static constexpr int card_bottom = 107;
    static constexpr int lines_per_page = 4;
    static_assert(first_line_y + (lines_per_page - 1) * line_height + glyph_height <= card_bottom,
        "Inbox lines must fit inside the message card");
    const size_t text_len = strlen(text);
    size_t pos = offset > text_len ? 0 : offset;

    display.setTextSize(1);

    for (int line_num = 0; line_num < lines_per_page && pos < text_len; line_num++) {
      while (pos < text_len && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r')) pos++;
      if (pos >= text_len) break;
      if (text[pos] == '\n') {
        pos++;
        continue;
      }

      const size_t line_start = pos;
      size_t line_end = line_start;
      while (line_end < text_len && line_end - line_start < chars_per_line &&
          text[line_end] != '\n' && text[line_end] != '\r') line_end++;

      size_t next = line_end;
      if (line_end < text_len && text[line_end] != '\n' && text[line_end] != '\r') {
        size_t split = line_end;
        while (split > line_start && text[split - 1] != ' ' && text[split - 1] != '\t') split--;
        if (split > line_start) {
          line_end = split - 1;
          while (line_end > line_start &&
              (text[line_end - 1] == ' ' || text[line_end - 1] == '\t')) line_end--;
          next = split;
        }
      } else if (line_end < text_len) {
        next = line_end + 1;
      }

      char line[chars_per_line + 1];
      const size_t line_len = line_end - line_start;
      memcpy(line, text + line_start, line_len);
      line[line_len] = 0;
      display.setCursor(10, first_line_y + line_num * line_height);
      display.print(line);
      pos = next;
    }
    return pos;
  }
#endif

  void formatOrigin(char* dest, size_t dest_size, const MsgEntry& entry) const {
    if (entry.path_len == 0xFF) {
      snprintf(dest, dest_size, "(D) %s:", entry.source);
    } else {
      snprintf(dest, dest_size, "(%u) %s:", (unsigned)entry.path_len, entry.source);
    }
  }

public:
  MsgPreviewScreen(UITask* task, mesh::RTCClock* rtc) : _task(task), _rtc(rtc) { num_unread = 0; }

  int addPreview(uint8_t path_len, const char* from_name, const char* msg) {
    head = (head + 1) % UI_MAX_UNREAD_MSGS;
    if (num_unread < UI_MAX_UNREAD_MSGS) {
      num_unread++;
#ifdef NEONPOCKET_UI
    } else {
      overflowed = true;
#endif
    }

    auto p = &unread[head];
    p->timestamp = _rtc->getCurrentTime();
    p->path_len = path_len;
    StrHelper::strncpy(p->source, from_name, sizeof(p->source));
    StrHelper::strncpy(p->msg, msg, sizeof(p->msg));
    return num_unread;
  }

#ifdef NEONPOCKET_UI
  bool hasOverflowed() const { return overflowed; }
#endif

  int render(DisplayDriver& display) override {
#ifdef NEONPOCKET_UI
    char tmp[20];
    auto p = &unread[head];
    int secs = _rtc->getCurrentTime() - p->timestamp;
    if (secs < 0) secs = 0;
    if (secs < 60) {
      snprintf(tmp, sizeof(tmp), "%ds", secs);
    } else if (secs < 3600) {
      snprintf(tmp, sizeof(tmp), "%dm", secs / 60);
    } else {
      snprintf(tmp, sizeof(tmp), "%dh", secs / 3600);
    }

    display.setTextSize(1);
    display.setColor(NEON_GREEN);
    display.setCursor(4, 1);
    display.print("INBOX");
    display.setColor(NEON_YELLOW);
    char count[20];
    snprintf(count, sizeof(count), "%d%s unread", num_unread, overflowed ? "+" : "");
    display.drawTextCentered(display.width() / 2, 1, count);
    display.setColor(NEON_LIGHT);
    display.drawTextRightAlign(display.width() - 4, 1, tmp);
    display.setColor(NEON_BLUE);
    display.fillRect(0, 17, display.width(), 2);

    char origin[48];
    formatOrigin(origin, sizeof(origin), *p);
    char filtered_origin[sizeof(origin)];
    display.translateUTF8ToBlocks(filtered_origin, origin, sizeof(filtered_origin));
    display.setColor(NEON_YELLOW);
    display.drawTextEllipsized(5, 23, display.width() - 10, filtered_origin);

    display.setColor(NEON_BLUE);
    display.drawRect(4, 42, display.width() - 8, 66);
    display.setCursor(10, 49);
    display.setColor(NEON_LIGHT);
    char filtered_msg[sizeof(p->msg)];
    display.translateUTF8ToBlocks(filtered_msg, p->msg, sizeof(filtered_msg));
    if (page_head != head) {
      page_head = head;
      page_offset = 0;
    }
    next_page_offset = renderMessagePage(display, filtered_msg, page_offset);
    page_has_more = filtered_msg[next_page_offset] != 0;

    display.setColor(NEON_BLUE);
    display.setCursor(4, 113);
    display.print(page_has_more ? "CLICK MORE" : "CLICK NEXT");
    display.drawTextRightAlign(display.width() - 4, 113, "3X CLEAR ALL");
    return 1000;
#else
    char tmp[16];
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.setColor(NEON_GREEN);
    sprintf(tmp, "Unread: %d", num_unread);
    display.print(tmp);

    auto p = &unread[head];

    int secs = _rtc->getCurrentTime() - p->timestamp;
    if (secs < 60) {
      sprintf(tmp, "%ds", secs);
    } else if (secs < 60*60) {
      sprintf(tmp, "%dm", secs / 60);
    } else {
      sprintf(tmp, "%dh", secs / (60*60));
    }
    display.setCursor(display.width() - display.getTextWidth(tmp) - 2, 0);
    display.print(tmp);

    display.drawRect(0, 11, display.width(), 1);  // horiz line

    display.setCursor(0, 14);
    display.setColor(NEON_YELLOW);
    char origin[48];
    formatOrigin(origin, sizeof(origin), *p);
    char filtered_origin[sizeof(origin)];
    display.translateUTF8ToBlocks(filtered_origin, origin, sizeof(filtered_origin));
    display.print(filtered_origin);

    display.setCursor(0, 25);
    display.setColor(NEON_LIGHT);
    char filtered_msg[sizeof(p->msg)];
    display.translateUTF8ToBlocks(filtered_msg, p->msg, sizeof(filtered_msg));
    display.printWordWrap(filtered_msg, display.width());

#if AUTO_OFF_MILLIS==0 // probably e-ink
    return 10000; // 10 s
#else
    return 1000;  // next render after 1000 ms
#endif
#endif
  }

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) {
#ifdef NEONPOCKET_UI
      if (page_has_more) {
        page_offset = next_page_offset;
        return true;
      }
      page_offset = next_page_offset = 0;
      page_head = -1;
      page_has_more = false;
#endif
      head = (head + UI_MAX_UNREAD_MSGS - 1) % UI_MAX_UNREAD_MSGS;
      num_unread--;
#ifdef NEONPOCKET_UI
      if (num_unread > 0) {
        const auto p = &unread[head];
        _task->setLocalUnread(num_unread, p->source, p->msg, overflowed);
      } else {
        overflowed = false;
        _task->setLocalUnread(0);
      }
#endif
      if (num_unread == 0) {
        _task->gotoHomeScreen();
      }
      return true;
    }
    if (c == KEY_ENTER) {
      num_unread = 0;  // clear unread queue
#ifdef NEONPOCKET_UI
      overflowed = false;
      page_offset = next_page_offset = 0;
      page_head = -1;
      page_has_more = false;
      _task->setLocalUnread(0);
#endif
      _task->gotoHomeScreen();
      return true;
    }
    return false;
  }
};

#ifdef NEONPOCKET_RCC6_UI_EXTENSIONS
void UITask::sampleDiagnostics() {
  _cached_uptime_seconds = millis() / 1000;
  _cached_rx_packets = radio_driver.getPacketsRecv();
  _cached_tx_packets = radio_driver.getPacketsSent();
  _cached_rx_errors = radio_driver.getPacketsRecvErrors();
  _cached_noise_floor = radio_driver.getNoiseFloor();
  _cached_heap_free = ESP.getFreeHeap();
  _cached_heap_max = ESP.getMaxAllocHeap();
}
#endif

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _display = display;
  _sensors = sensors;
  _auto_off = millis() + displayAutoOffMillis();

#if defined(PIN_USER_BTN)
  user_btn.begin();
#endif
#if defined(PIN_USER_BTN_ANA)
  analog_btn.begin();
#endif

  _node_prefs = node_prefs;

  if (_display != NULL) {
    _display->turnOn();
  }

#ifdef PIN_BUZZER
  buzzer.begin();
  buzzer.quiet(_node_prefs->buzzer_quiet);
  buzzer.startup();
#endif

#ifdef PIN_VIBRATION
  vibration.begin();
#endif

  ui_started_at = millis();
  _alert_expiry = 0;
#ifdef NEONPOCKET_UI
  _connection_known = false;
  _wake_pending = false;
  _pending_pulse_duration = 0;
  _pending_alert_duration = 0;
  _manual_advert_until = 0;
  _radio_sample_known = false;
  _battery_low_warning = false;
  _next_message_color = NEON_YELLOW;
  _cached_batt_millivolts = AbstractUITask::getBattMilliVolts();
  _next_batt_sample = millis() + 60000;
  if (_cached_batt_millivolts > 0 && _cached_batt_millivolts <= 3450) {
    _battery_low_warning = true;
    showAlert("Battery low", 1200, NEON_RED);
  }
#ifdef NEONPOCKET_RCC6_UI_EXTENSIONS
  sampleDiagnostics();
  _next_diag_sample = millis() + 5000;
#endif
#endif

#ifdef NEONPOCKET_ULTIMATE
  splash = nullptr;
  home = new UltimateUIScreen(this, &rtc_clock, sensors, node_prefs);
  msg_preview = home;
#else
  splash = new SplashScreen(this);
  home = new HomeScreen(this, &rtc_clock, sensors, node_prefs);
  msg_preview = new MsgPreviewScreen(this, &rtc_clock);
#endif
#ifdef NEONPOCKET_ULTIMATE
  setCurrScreen(home);
#else
  setCurrScreen(splash);
#endif
}

#if defined(NEONPOCKET_UI) && defined(NEONPOCKET_ULTIMATE)
void UITask::gotoMsgPreviewScreen() {
  static_cast<UltimateUIScreen*>(home)->openInbox();
  setCurrScreen(home);
}
#endif

void UITask::showAlert(const char* text, int duration_millis, ColorVal color) {
  strcpy(_alert, text);
#ifdef NEONPOCKET_UI
  const unsigned long now = millis();
  _alert_color = color;
  if (_display != NULL && !_display->isOn()) {
    _pending_alert_duration = duration_millis;
    _alert_started = 0;
    _alert_expiry = 0;
    _wake_pending = true;
  } else {
    _pending_alert_duration = 0;
    _alert_started = now;
    _alert_expiry = now + duration_millis;
  }
  if (color != NEON_LIGHT) startNeonPulse(color);
#else
  _alert_expiry = millis() + duration_millis;
  (void)color;
#endif
}

#ifdef NEONPOCKET_UI
void UITask::startNeonPulse(ColorVal color, unsigned long duration_millis) {
  const unsigned long now = millis();
  _pulse_color = color;
  if (_display != NULL && !_display->isOn()) {
    _pending_pulse_duration = duration_millis;
    _pulse_started = 0;
    _pulse_until = 0;
    _wake_pending = true;
  } else {
    _pending_pulse_duration = 0;
    _pulse_started = now;
    _pulse_until = now + duration_millis;
    if (_display != NULL) _auto_off = now + displayAutoOffMillis();
  }
  _next_refresh = 0;
}

bool UITask::isPowerConfirmArmed() const {
  return _power_confirm_until != 0 &&
      (int32_t)(_power_confirm_until - millis()) >= 0;
}

bool UITask::handleNeonInput(char c) {
  if (c != KEY_ENTER) {
    _power_confirm_until = 0;
    return false;
  }

  const bool can_confirm = isPowerConfirmArmed() && _display != NULL && _display->isOn() &&
      curr == home &&
#ifdef NEONPOCKET_ULTIMATE
      static_cast<UltimateUIScreen*>(home)->isPowerPage();
#else
      static_cast<HomeScreen*>(home)->neonIsPowerPage();
#endif
  if (can_confirm) {
    _power_confirm_until = 0;
    gotoHomeScreen();
#ifdef NEONPOCKET_ULTIMATE
    static_cast<UltimateUIScreen*>(home)->requestShutdown();
#else
    static_cast<HomeScreen*>(home)->neonRequestShutdown();
#endif
  } else {
    _power_confirm_until = millis() + NEON_POWER_CONFIRM_MILLIS;
    gotoHomeScreen();
#ifdef NEONPOCKET_ULTIMATE
    static_cast<UltimateUIScreen*>(home)->showPowerConfirm();
#else
    static_cast<HomeScreen*>(home)->neonShowPowerConfirm();
#endif
    startNeonPulse(NEON_RED, NEON_TRANSITION_MILLIS);
  }
  _next_refresh = 0;
  return true;
}
#endif

void UITask::notify(UIEventType t) {
#ifdef NEONPOCKET_UI
  switch (t) {
    case UIEventType::contactMessage: startNeonPulse(NEON_YELLOW); break;
    case UIEventType::channelMessage:
    case UIEventType::roomMessage: startNeonPulse(NEON_BLUE); break;
    case UIEventType::newContactMessage:
      showAlert("New nearby", 900, NEON_ORANGE);
      break;
    case UIEventType::ack: startNeonPulse(NEON_GREEN, 300); break;
    case UIEventType::none: break;
  }
#endif
#if defined(PIN_BUZZER)
switch(t){
  case UIEventType::contactMessage:
    // gemini's pick
    buzzer.play("MsgRcv3:d=4,o=6,b=200:32e,32g,32b,16c7");
    break;
  case UIEventType::channelMessage:
    buzzer.play("kerplop:d=16,o=6,b=120:32g#,32c#");
    break;
  case UIEventType::ack:
    buzzer.play("ack:d=32,o=8,b=120:c");
    break;
  case UIEventType::roomMessage:
  case UIEventType::newContactMessage:
  case UIEventType::none:
  default:
    break;
}
#endif

#ifdef PIN_VIBRATION
  // Trigger vibration for all UI events except none
  if (t != UIEventType::none) {
    vibration.trigger();
  }
#endif
}

#ifdef NEONPOCKET_UI
void UITask::onNewContactVisual() {
  showAlert("New nearby", 900, NEON_ORANGE);
}

void UITask::onRadioEvent(UIRadioEvent event, uint8_t payload_type,
    int16_t rssi_dbm, int16_t snr_quarter_db) {
  if (event == UIRadioEvent::Rx) {
    _radio_rssi_dbm = rssi_dbm;
    _radio_snr_quarter_db = snr_quarter_db;
    _radio_sample_known = true;
    _radio_rx_pending = true;
  } else {
    _radio_tx_event = event;
    _radio_tx_payload_type = payload_type;
    _radio_tx_pending = true;
  }
}
#endif

void UITask::msgRead(int msgcount) {
#ifdef NEONPOCKET_UI
  // Companion sync means "delivered to the phone", not "read by the user".
  // Keep the on-device unread count until it is acknowledged on the display.
  (void) msgcount;
#else
  _msgcount = msgcount;
  if (msgcount == 0) {
    gotoHomeScreen();
  }
#endif
}

#ifdef NEONPOCKET_UI
void UITask::setLocalUnread(int count, const char* sender, const char* preview, bool overflow) {
  _msgcount = count;
  _unread_overflow = overflow;
  if (count == 0) {
    _latest_sender[0] = 0;
    _latest_preview[0] = 0;
  } else if (sender != nullptr && preview != nullptr) {
    StrHelper::strncpy(_latest_sender, sender, sizeof(_latest_sender));
    StrHelper::strncpy(_latest_preview, preview, sizeof(_latest_preview));
  }
  _next_refresh = 0;
}

void UITask::newMsgWithEvent(uint8_t path_len, const char* from_name, const char* text,
    int msgcount, UIEventType type) {
  const bool is_channel = type == UIEventType::channelMessage || type == UIEventType::roomMessage;
  _next_message_color = is_channel ? NEON_BLUE : NEON_YELLOW;
  if (is_channel && from_name[0] != '#') {
    char channel_label[sizeof(_latest_sender)];
    snprintf(channel_label, sizeof(channel_label), "#%s", from_name);
    newMsg(path_len, channel_label, text, msgcount);
  } else {
    newMsg(path_len, from_name, text, msgcount);
  }
}
#endif

void UITask::newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) {
#ifndef NEONPOCKET_UI
  _msgcount = msgcount;
#endif

#ifdef NEONPOCKET_UI
  const bool display_was_off = _display != NULL && !_display->isOn();
  StrHelper::strncpy(_latest_sender, from_name, sizeof(_latest_sender));
  StrHelper::strncpy(_latest_preview, text, sizeof(_latest_preview));
  const ColorVal message_color = _next_message_color;
  _next_message_color = NEON_YELLOW;
  startNeonPulse(message_color);
#endif

#ifdef NEONPOCKET_ULTIMATE
  _msgcount = ultimate_service.getSnapshot().unread_count;
  const bool hide_locked = ultimate_service.getSettings().private_notifications && display_was_off;
  if (hide_locked) _private_notification_locked = true;
  if (!hide_locked) gotoMsgPreviewScreen();
#else
  const int local_unread = ((MsgPreviewScreen *) msg_preview)->addPreview(path_len, from_name, text);
#ifdef NEONPOCKET_UI
  _msgcount = local_unread;
  _unread_overflow = ((MsgPreviewScreen *) msg_preview)->hasOverflowed();
#endif
  setCurrScreen(msg_preview);
#endif

  if (_display != NULL) {
#ifndef NEONPOCKET_UI
    if (!_display->isOn() && !hasConnection()) {
      _display->turnOn();
    }
#endif
    if (_display->isOn()) {
    _auto_off = millis() + displayAutoOffMillis();  // extend the auto-off timer
    _next_refresh = 100;  // trigger refresh
    }
  }
}

void UITask::userLedHandler() {
#ifdef PIN_STATUS_LED
  int cur_time = millis();
  if (cur_time > next_led_change) {
    if (led_state == 0) {
      led_state = 1;
      if (_msgcount > 0) {
        last_led_increment = LED_ON_MSG_MILLIS;
      } else {
        last_led_increment = LED_ON_MILLIS;
      }
      next_led_change = cur_time + last_led_increment;
    } else {
      led_state = 0;
      next_led_change = cur_time + LED_CYCLE_MILLIS - last_led_increment;
    }
    digitalWrite(PIN_STATUS_LED, led_state == LED_STATE_ON);
  }
#endif
}

void UITask::setCurrScreen(UIScreen* c) {
  curr = c;
  _next_refresh = 100;
}

/*
  hardware-agnostic pre-shutdown activity should be done here
*/
void UITask::shutdown(bool restart){

  #ifdef PIN_BUZZER
  /* note: we have a choice here -
     we can do a blocking buzzer.loop() with non-deterministic consequences
     or we can set a flag and delay the shutdown for a couple of seconds
     while a non-blocking buzzer.loop() plays out in UITask::loop()
  */
  buzzer.shutdown();
  uint32_t buzzer_timer = millis(); // fail-safe shutdown
  while (buzzer.isPlaying() && (millis() - 2500) < buzzer_timer)
    buzzer.loop();

  #endif // PIN_BUZZER

  if (restart) {
    _board->reboot();
  } else {
    // Power off board including radio, display, GPS and components
    _board->powerOff();
  }
}

bool UITask::isButtonPressed() const {
#ifdef PIN_USER_BTN
  return user_btn.isPressed();
#else
  return false;
#endif
}

void UITask::loop() {
  char c = 0;
#ifdef NEONPOCKET_UI
  const unsigned long event_now = millis();
#ifdef NEONPOCKET_ULTIMATE
  _msgcount = ultimate_service.getSnapshot().unread_count;
#endif
  if (_power_confirm_until != 0 && (int32_t)(event_now - _power_confirm_until) >= 0) {
    _power_confirm_until = 0;
    _next_refresh = 0;
  }
  if (_wake_pending && _display != NULL) {
    _wake_pending = false;
    if (!_display->isOn()) _display->turnOn();
    const unsigned long wake_now = millis();
    _auto_off = wake_now + displayAutoOffMillis();
    if (_pending_pulse_duration > 0) {
      _pulse_started = wake_now;
      _pulse_until = wake_now + _pending_pulse_duration;
      _pending_pulse_duration = 0;
    }
    if (_pending_alert_duration > 0) {
      _alert_started = wake_now;
      _alert_expiry = wake_now + _pending_alert_duration;
      _pending_alert_duration = 0;
    }
    _next_refresh = 0;
  }
  if ((int32_t)(event_now - _next_batt_sample) >= 0) {
    _cached_batt_millivolts = AbstractUITask::getBattMilliVolts();
    _next_batt_sample = millis() + 60000;
    if (_cached_batt_millivolts > 0) {
      if (!_battery_low_warning && _cached_batt_millivolts <= 3450) {
        _battery_low_warning = true;
        showAlert("Battery low", 1200, NEON_RED);
      } else if (_battery_low_warning && _cached_batt_millivolts >= 3600) {
        _battery_low_warning = false;
      }
    } else {
      _battery_low_warning = false;
    }
    _next_refresh = 0;
  }
#ifdef NEONPOCKET_RCC6_UI_EXTENSIONS
  if ((int32_t)(event_now - _next_diag_sample) >= 0) {
    sampleDiagnostics();
    _next_diag_sample = millis() + 5000;
    _next_refresh = 0;
  }
#endif
  if (_radio_rx_pending) {
    _radio_rx_pending = false;
    if (_display != NULL && _display->isOn() &&
        (event_now - _last_rx_pulse >= 250 || _last_rx_pulse == 0)) {
      _last_rx_pulse = event_now;
      _next_refresh = 0;  // refresh live RF metrics without waking/flashing on every packet
    }
  }
  if (_radio_tx_pending) {
    const UIRadioEvent event = _radio_tx_event;
    const uint8_t payload_type = _radio_tx_payload_type;
    _radio_tx_pending = false;
    const bool ok = event == UIRadioEvent::TxComplete;
    if (payload_type == PAYLOAD_TYPE_ADVERT) {
      const bool armed = _manual_advert_until != 0 &&
          (int32_t)(_manual_advert_until - event_now) >= 0;
      _manual_advert_until = 0;
      if (armed) {
        showAlert(ok ? "Mesh advert sent" : "Advert failed", 1000,
            ok ? NEON_GREEN : NEON_RED);
      }
    }
  }
  const bool connected = hasConnection();
  if (!_connection_known) {
    _connection_known = true;
    _last_connection = connected;
  } else if (connected != _last_connection) {
    _last_connection = connected;
#ifdef RCC6_WEB_AP
    showAlert(connected ? "Network client" : "Client disconnected", 900,
#else
    showAlert(connected ? "Phone connected" : "Phone disconnected", 900,
#endif
        connected ? NEON_GREEN : NEON_RED);
  }
#endif
#if UI_HAS_JOYSTICK
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_ENTER);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_ENTER);  // REVISIT: could be mapped to different key code
  }
  ev = joystick_left.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_LEFT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_LEFT);
  }
  ev = joystick_right.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_RIGHT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_RIGHT);
  }
  ev = back_btn.check();
  if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
    c = handleTripleClick(KEY_SELECT);
  }
#elif defined(PIN_USER_BTN)
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_NEXT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_ENTER);
  } else if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
    c = handleDoubleClick(KEY_PREV);
  } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
    c = handleTripleClick(KEY_SELECT);
  }
#endif
#if defined(UI_HAS_ROTARY_INPUT)
  RotaryInputEvent rotaryEv = rotary_input.poll();
  if (c == 0 && _display != NULL && _display->isOn()) {
    if (rotaryEv == RotaryInputEvent::Next) {
      c = KEY_NEXT;
    } else if (rotaryEv == RotaryInputEvent::Prev) {
      c = KEY_PREV;
    }
  }
#endif
#if defined(PIN_USER_BTN_ANA)
  if (abs(millis() - _analogue_pin_read_millis) > 10) {
    int ev = analog_btn.check();
    if (ev == BUTTON_EVENT_CLICK) {
      c = checkDisplayOn(KEY_NEXT);
    } else if (ev == BUTTON_EVENT_LONG_PRESS) {
      c = handleLongPress(KEY_ENTER);
    } else if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
      c = handleDoubleClick(KEY_PREV);
    } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
      c = handleTripleClick(KEY_SELECT);
    }
    _analogue_pin_read_millis = millis();
  }
#endif
#if defined(BACKLIGHT_BTN)
  if (millis() > next_backlight_btn_check) {
    bool touch_state = digitalRead(PIN_BUTTON2);
#if defined(DISP_BACKLIGHT)
    digitalWrite(DISP_BACKLIGHT, !touch_state);
#elif defined(EXP_PIN_BACKLIGHT)
    expander.digitalWrite(EXP_PIN_BACKLIGHT, !touch_state);
#endif
    next_backlight_btn_check = millis() + 300;
  }
#endif

  if (c != 0 && curr) {
#ifdef NEONPOCKET_UI
    if (!handleNeonInput(c)) curr->handleInput(c);
#else
    curr->handleInput(c);
#endif
    _auto_off = millis() + displayAutoOffMillis();   // extend auto-off timer
    _next_refresh = 100;  // trigger refresh
  }

  userLedHandler();

#ifdef PIN_BUZZER
  if (buzzer.isPlaying())  buzzer.loop();
#endif

  if (curr) curr->poll();

  if (_display != NULL && _display->isOn()) {
    if (millis() >= _next_refresh && curr) {
      _display->startFrame();
      int delay_millis = curr->render(*_display);
#ifdef NEONPOCKET_UI
      const unsigned long now = millis();
      const bool alert_active = now < _alert_expiry;
      const bool alert_animating = alert_active && now - _alert_started < 300;
      const bool pulse_active = now < _pulse_until;
      if (alert_active) {
        const unsigned long elapsed = now - _alert_started;
        const int slide = elapsed >= 300 ? 0 : (int)((300 - elapsed) * 16 / 300);
        const int y = 44 + slide;
        _display->setTextSize(1);
        _display->setColor(NEON_DARK);
        _display->fillRect(12, y, _display->width() - 24, 40);
        _display->setColor(_alert_color);
        _display->drawRect(12, y, _display->width() - 24, 40);
        _display->drawTextCentered(_display->width() / 2, y + 12, _alert);
      }
      if (pulse_active && (((now - _pulse_started) / neonFrameMillis()) % 2 == 0)) {
        _display->setColor(_pulse_color);
        _display->fillRect(0, 0, _display->width(), 3);
        _display->fillRect(0, _display->height() - 3, _display->width(), 3);
      }
      if (alert_animating || pulse_active) {
        _next_refresh = now + neonFrameMillis();
      } else if (alert_active) {
        const unsigned long page_refresh = now + delay_millis;
        _next_refresh = (int32_t)(page_refresh - _alert_expiry) < 0
            ? page_refresh : _alert_expiry;
      } else {
        _next_refresh = now + delay_millis;
      }
#else
      if (millis() < _alert_expiry) {  // render alert popup
        _display->setTextSize(1);
        int y = _display->height() / 3;
        int p = _display->height() / 32;
        _display->setColor(NEON_DARK);
        _display->fillRect(p, y, _display->width() - p*2, y);
        _display->setColor(NEON_LIGHT);  // draw box border
        _display->drawRect(p, y, _display->width() - p*2, y);
        _display->drawTextCentered(_display->width() / 2, y + p*3, _alert);
        _next_refresh = _alert_expiry;   // will need refresh when alert is dismissed
      } else {
        _next_refresh = millis() + delay_millis;
      }
#endif
      _display->endFrame();
#ifdef NEONPOCKET_ULTIMATE
      NV3001BDisplay* native_display = static_cast<NV3001BDisplay*>(_display);
      ultimate_service.setDisplayTransfer(native_display->lastFlushMicros(),
                                          native_display->lastTilesSent());
#endif
    }
#if AUTO_OFF_MILLIS > 0
#ifdef KEEP_DISPLAY_ON_USB
    // Opt-in: refresh the auto-off deadline while externally powered, so the
    // timer counts from the moment external power is removed. Off by default
    // because OLED panels burn in quickly; only enable for LCD targets or
    // where the display is replaceable.
    if (board.isExternalPowered()) {
      _auto_off = millis() + displayAutoOffMillis();
    }
#endif
    if ((int32_t)(millis() - _auto_off) >= 0) {
#ifdef NEONPOCKET_UI
      _power_confirm_until = 0;
#endif
      _display->turnOff();
    }
#endif
  }

#ifdef PIN_VIBRATION
  vibration.loop();
#endif

#ifdef AUTO_SHUTDOWN_MILLIVOLTS
  if (millis() > next_batt_chck) {
    uint16_t milliVolts = getBattMilliVolts();
    if (milliVolts > 0 && milliVolts < AUTO_SHUTDOWN_MILLIVOLTS) {
      if(!board.isExternalPowered()) {
        if (_display != NULL) {
#ifdef NEONPOCKET_UI
            if (!_display->isOn()) _display->turnOn();
            const bool animate = !_display->isEink();
            const int frames = animate ? 6 : 1;
            char voltage[24];
            snprintf(voltage, sizeof(voltage), "%u mV", milliVolts);
            for (int frame = 0; frame < frames; frame++) {
              _display->startFrame();
              if ((frame % 2) == 0) {
                _display->setColor(NEON_RED);
                _display->fillRect(0, 0, _display->width(), 3);
                _display->fillRect(0, _display->height() - 3, _display->width(), 3);
              }
              _display->setTextSize(2);
              _display->setColor(NEON_RED);
              _display->drawTextCentered(_display->width() / 2, 30, "LOW BATTERY");
              _display->setTextSize(1);
              _display->setColor(NEON_LIGHT);
              _display->drawTextCentered(_display->width() / 2, 65, voltage);
              _display->drawTextCentered(_display->width() / 2, 87, "Shutting down");
              _display->endFrame();
              if (animate) delay(NEON_FRAME_MILLIS);
            }
            if (animate) delay(2400);
#else
            _display->startFrame();
            _display->setTextSize(2);
            _display->setColor(NEON_RED);
            _display->drawTextCentered(_display->width() / 2, 20, "Low Battery.");
            _display->drawTextCentered(_display->width() / 2, 40, "Shutting Down!");
            _display->endFrame();
            if (_display->isEink() == false) { delay(3000); }
#endif
        }
        shutdown();
      }
    }
    next_batt_chck = millis() + 8000;
  }
#endif
}

char UITask::checkDisplayOn(char c) {
  if (_display != NULL) {
    if (!_display->isOn()) {
      _display->turnOn();   // turn display on and consume event
      c = 0;
    }
#ifdef NEONPOCKET_ULTIMATE
    else if (_private_notification_locked) {
      _private_notification_locked = false;
      c = 0;  // first gesture reveals private notification content only
    }
#endif
    _auto_off = millis() + displayAutoOffMillis();   // extend auto-off timer
    _next_refresh = 0;  // trigger refresh
  }
  return c;
}

char UITask::handleLongPress(char c) {
#ifdef NEONPOCKET_ULTIMATE
  c = checkDisplayOn(c);
  if (c == 0) return 0;
#endif
  if (millis() - ui_started_at < 8000) {   // long press in first 8 seconds since startup -> CLI/rescue
    the_mesh.enterCLIRescue();
    c = 0;   // consume event
  }
  return c;
}

char UITask::handleDoubleClick(char c) {
  MESH_DEBUG_PRINTLN("UITask: double-click triggered");
#ifdef NEONPOCKET_UI
  _power_confirm_until = 0;
  c = checkDisplayOn(KEY_ENTER);
  if (c != 0 && curr) curr->handleInput(c);
  return 0;
#else
  checkDisplayOn(c);
  return c;
#endif
}

char UITask::handleTripleClick(char c) {
  MESH_DEBUG_PRINTLN("UITask: triple click triggered");
#ifdef NEONPOCKET_UI
  _power_confirm_until = 0;
  c = checkDisplayOn(KEY_SELECT);
  if (c == 0) return 0;
#ifdef NEONPOCKET_ULTIMATE
  static_cast<UltimateUIScreen*>(home)->handleTriplePress();
#else
  if (curr == msg_preview) curr->handleInput(KEY_ENTER);
  static_cast<HomeScreen*>(home)->neonGoHome();
  gotoHomeScreen();
#endif
#else
  checkDisplayOn(c);
  toggleBuzzer();
#endif
  c = 0;
  return c;
}

bool UITask::getGPSState() {
  if (_sensors != NULL) {
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
        return !strcmp(_sensors->getSettingValue(i), "1");
      }
    }
  }
  return false;
}

void UITask::toggleGPS() {
    if (_sensors != NULL) {
    // toggle GPS on/off
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
        if (strcmp(_sensors->getSettingValue(i), "1") == 0) {
          _sensors->setSettingValue("gps", "0");
          _node_prefs->gps_enabled = 0;
          notify(UIEventType::ack);
        } else {
          _sensors->setSettingValue("gps", "1");
          _node_prefs->gps_enabled = 1;
          notify(UIEventType::ack);
        }
        the_mesh.savePrefs();
        showAlert(_node_prefs->gps_enabled ? "GPS: Enabled" : "GPS: Disabled", 800);
        _next_refresh = 0;
        break;
      }
    }
  }
}

void UITask::toggleBuzzer() {
    // Toggle buzzer quiet mode
  #ifdef PIN_BUZZER
    if (buzzer.isQuiet()) {
      buzzer.quiet(false);
      notify(UIEventType::ack);
    } else {
      buzzer.quiet(true);
    }
    _node_prefs->buzzer_quiet = buzzer.isQuiet();
    the_mesh.savePrefs();
    showAlert(buzzer.isQuiet() ? "Buzzer: OFF" : "Buzzer: ON", 800);
    _next_refresh = 0;  // trigger refresh
  #endif
}
