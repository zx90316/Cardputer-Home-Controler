#include <Arduino.h>
#include <M5Cardputer.h>
#include <WiFi.h>

#include "ConfigStore.h"
#include "SetupPortal.h"
#include "adapters/DysonAdapter.h"
#include "adapters/IrAdapter.h"
#include "adapters/TapoAdapter.h"
#include "core/CommandDispatcher.h"
#include "core/Models.h"
#include "core/ProtocolHelpers.h"
#include "core/TimeUtils.h"

using namespace chc;

namespace {

QueueHandle_t eventQueue;
SemaphoreHandle_t stateMutex;
SemaphoreHandle_t commandMutex;
CommandDispatcher<24> commandDispatcher;
AppConfig appConfig;
ConfigStore configStore;
SetupPortal setupPortal;
IrAdapter irAdapter;
DysonAdapter dysonAdapter;
TapoAdapter tapoAdapter;
AcState sharedAc;
DysonState sharedDyson;
LightState sharedLight;
AdapterHealth sharedAcHealth;
AdapterHealth sharedDysonHealth;
AdapterHealth sharedLightHealth;
struct SystemState {
  bool wifiConnected{false};
  int32_t rssi{0};
  char ip[16]{"0.0.0.0"};
} sharedSystem;
volatile DeviceId selectedDevice = DeviceId::Ac;
uint8_t selectedItem[3]{};
uint32_t nextCommandId = 1;
bool setupMode = false;
enum class UiPage : uint8_t { Quick, Ac, Dyson, DysonAir, Light, Diagnostics };
UiPage currentPage = UiPage::Quick;
char lastFeedback[48] = "就緒";
uint16_t feedbackColor = TFT_DARKGREY;
EventResult lastDeviceResult[3]{EventResult::Succeeded, EventResult::Offline, EventResult::Offline};
M5Canvas uiCanvas(&M5Cardputer.Display);
constexpr const char* kFirmwareVersion = "1.0.0-rc1";

const char* connectionText(ConnectionState state) {
  switch (state) {
    case ConnectionState::Online: return "ONLINE";
    case ConnectionState::Degraded: return "PARTIAL";
    case ConnectionState::Connecting: return "CONNECT";
    case ConnectionState::AuthFailed: return "AUTH ERR";
    case ConnectionState::Offline: return "OFFLINE";
    default: return "NO CONFIG";
  }
}

bool enqueue(DeviceId device, CommandKind kind, int v1 = 0, int v2 = 0, int v3 = 0) {
  DeviceCommand command{device, kind, v1, v2, v3, nextCommandId++};
  xSemaphoreTake(commandMutex, portMAX_DELAY);
  const bool accepted = commandDispatcher.push(command);
  xSemaphoreGive(commandMutex);
  if (!accepted) {
    strncpy(lastFeedback, "命令佇列已滿", sizeof(lastFeedback) - 1);
    feedbackColor = TFT_RED;
  } else {
    strncpy(lastFeedback, "命令已送出，等待設備回報", sizeof(lastFeedback) - 1);
    feedbackColor = TFT_YELLOW;
    lastDeviceResult[static_cast<uint8_t>(device) - 1] = EventResult::Pending;
  }
  lastFeedback[sizeof(lastFeedback) - 1] = '\0';
  return accepted;
}

bool latestCommand(DeviceId device, CommandKind kind, DeviceCommand& command) {
  xSemaphoreTake(commandMutex, portMAX_DELAY);
  const bool found = commandDispatcher.latest(device, kind, command);
  xSemaphoreGive(commandMutex);
  return found;
}

int effectiveValue(DeviceId device, CommandKind kind, int fallback, uint8_t field = 1) {
  DeviceCommand command;
  if (!latestCommand(device, kind, command)) return fallback;
  return field == 2 ? command.value2 : field == 3 ? command.value3 : command.value1;
}

int nextTimerValue(int current) {
  if (current < 30) return 30;
  if (current < 60) return 60;
  if (current < 120) return 120;
  return 0;
}

uint8_t deviceIndex(DeviceId device) { return static_cast<uint8_t>(device) - 1; }
uint8_t itemCount(DeviceId device) {
  return device == DeviceId::Ac ? 12 : device == DeviceId::Dyson ? 9 : 7;
}
const char* onOff(bool value) { return value ? "開" : "關"; }
String timerText(int minutes) { return minutes > 0 ? String(minutes) + " 分" : "關閉"; }

const char* itemLabel(DeviceId device, uint8_t item) {
  static const char* ac[] = {"電源", "溫度", "模式", "風速", "上下擺風", "睡眠", "強力", "節能", "自體清潔", "面板燈", "開機計時", "關機計時"};
  static const char* dyson[] = {"電源", "風速", "自動模式", "左右擺風", "擺風範圍", "氣流方向", "夜間模式", "持續監測", "睡眠計時"};
  static const char* light[] = {"全部電源", "亮度", "色溫", "色相", "彩度", "動態效果", "內建預設"};
  return device == DeviceId::Ac ? ac[item] : device == DeviceId::Dyson ? dyson[item] : light[item];
}

String itemValue(DeviceId device, uint8_t item, const AcState& ac,
                 const DysonState& dyson, const LightState& light) {
  static const char* modes[] = {"自動", "冷氣", "除濕", "送風"};
  static const char* fans[] = {"自動", "低", "中", "高"};
  if (device == DeviceId::Ac) {
    switch (item) {
      case 0: return onOff(ac.power); case 1: return String(ac.temperature) + " °C";
      case 2: return modes[static_cast<uint8_t>(ac.mode) % 4];
      case 3: return fans[static_cast<uint8_t>(ac.fan) % 4];
      case 4: return onOff(ac.swingVertical); case 5: return onOff(ac.sleep);
      case 6: return onOff(ac.turbo); case 7: return onOff(ac.eco);
      case 8: return onOff(ac.clean); case 9: return onOff(ac.led);
      case 10: return timerText(ac.onTimerMinutes); default: return timerText(ac.offTimerMinutes);
    }
  }
  if (device == DeviceId::Dyson) {
    switch (item) {
      case 0: return onOff(dyson.power); case 1: return String(dyson.speed) + " / 10";
      case 2: return onOff(dyson.autoMode); case 3: return onOff(dyson.oscillation);
      case 4: return String(dyson.angleLow) + "–" + String(dyson.angleHigh) + "°";
      case 5: return dyson.frontAirflow ? "前方" : "後方";
      case 6: return onOff(dyson.nightMode); case 7: return onOff(dyson.continuousMonitoring);
      default: return timerText(dyson.sleepTimerMinutes);
    }
  }
  switch (item) {
    case 0: return light.mixed ? "混合" : onOff(light.power);
    case 1: return String(light.brightness) + "%";
    case 2: return light.colorTemperature ? String(light.colorTemperature) + " K" : "彩色模式";
    case 3: return String(light.hue) + "°"; case 4: return String(light.saturation) + "%";
    case 5: return light.effect == 1 ? "Party" : light.effect == 2 ? "Relax" : "關閉";
    default: return light.presetIndex >= 0 ? String(light.presetIndex + 1) : "未選";
  }
}

void drawHeader(const char* title, ConnectionState state) {
  auto& display = uiCanvas;
  display.setFont(&fonts::efontTW_12_b);
  display.setTextColor(TFT_CYAN, TFT_BLACK);
  display.setCursor(4, 2); display.print(title);
  display.setFont(&fonts::Font0);
  const char* status = connectionText(state);
  const int x = 236 - display.textWidth(status);
  display.setTextColor(state == ConnectionState::Online ? TFT_GREEN :
                       state == ConnectionState::Degraded ? TFT_YELLOW : TFT_ORANGE, TFT_BLACK);
  display.setCursor(x, 7); display.print(status);
  display.drawFastHLine(0, 20, 240, TFT_DARKGREY);
}

const char* acModeText(AcMode mode) {
  static const char* modes[] = {"自動", "冷氣", "除濕", "送風"};
  return modes[static_cast<uint8_t>(mode) % 4];
}

const char* resultIcon(EventResult result) {
  switch (result) {
    case EventResult::Pending: return "…";
    case EventResult::Succeeded: return "✓";
    case EventResult::PartiallySucceeded: return "△";
    default: return "!";
  }
}

uint16_t lightPreviewColor(const LightState& light) {
  int r = 255, g = 255, b = 255;
  if (light.colorTemperature) {
    const int warm = constrain(6500 - static_cast<int>(light.colorTemperature), 0, 4000);
    g = 255 - warm * 80 / 4000;
    b = 255 - warm * 170 / 4000;
  } else {
    const int h = light.hue % 360;
    const int c = 255 * light.saturation / 100;
    const int x = c * (60 - abs((h % 120) - 60)) / 60;
    const int m = 255 - c;
    if (h < 60) { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else { r = c; g = 0; b = x; }
    r += m; g += m; b += m;
  }
  const int brightness = max(20, static_cast<int>(light.brightness));
  return uiCanvas.color565(r * brightness / 100, g * brightness / 100, b * brightness / 100);
}

void drawQuickPage(const AcState& ac, const DysonState& dyson, const LightState& light,
                   const AdapterHealth& dysonHealth, const AdapterHealth& lightHealth,
                   const SystemState& system) {
  auto& display = uiCanvas;
  display.fillScreen(TFT_BLACK);
  const ConnectionState connection = !system.wifiConnected ? ConnectionState::Offline :
      (dysonHealth.connection == ConnectionState::Online && lightHealth.connection == ConnectionState::Online) ? ConnectionState::Online :
      (dysonHealth.connection == ConnectionState::Online || lightHealth.connection == ConnectionState::Online) ? ConnectionState::Degraded :
      ConnectionState::Offline;
  drawHeader("全體快捷", connection);
  display.setFont(&fonts::efontTW_10);

  display.fillRect(0, 22, 240, 27, 0x0841);
  display.setTextColor(TFT_CYAN, 0x0841);
  display.setCursor(4, 23); display.printf("%s 冷氣  Q電源  W− / E＋  R模式", resultIcon(lastDeviceResult[0]));
  display.setTextColor(TFT_WHITE, 0x0841);
  display.setCursor(4, 36);
  display.printf("狀態 %s   %u°C   ", ac.power ? "ON" : "OFF", ac.temperature);
  display.print(acModeText(ac.mode));

  display.fillRect(0, 50, 240, 29, 0x1002);
  display.setTextColor(TFT_ORANGE, 0x1002);
  display.setCursor(4, 51); display.printf("%s 風扇  A電源  S− / D＋  F擺動", resultIcon(lastDeviceResult[1]));
  display.setTextColor(TFT_WHITE, 0x1002);
  display.setCursor(4, 64);
  display.printf("G/H角度  %s %u級 %u–%u° ", dyson.power ? "ON" : "OFF", dyson.speed,
                 dyson.angleLow, dyson.angleHigh);
  display.print(dyson.oscillation ? "擺動" : "固定");

  display.fillRect(0, 80, 240, 28, 0x0208);
  display.setTextColor(TFT_GREENYELLOW, 0x0208);
  display.setCursor(4, 81); display.printf("%s 燈具  Z電源  X白/黃  C− / V＋", resultIcon(lastDeviceResult[2]));
  display.setTextColor(TFT_WHITE, 0x0208);
  display.setCursor(4, 94);
  display.printf("%u/%u在線  %s  %u%%  %uK", light.onlineDevices, light.totalDevices,
                 light.mixed ? "MIX" : (light.power ? "ON" : "OFF"), light.brightness,
                 light.colorTemperature);

  display.setTextColor(feedbackColor, TFT_BLACK);
  display.setCursor(4, 109); display.print(lastFeedback);
  display.fillRect(0, 120, 240, 15, TFT_NAVY);
  display.setTextColor(TFT_WHITE, TFT_NAVY);
  display.setCursor(3, 121); display.print("TAB詳細頁   0快捷頁");
  display.pushSprite(0, 0);
}

void drawDysonAirPage(const DysonState& dyson, const AdapterHealth& health) {
  auto& display = uiCanvas;
  display.fillScreen(TFT_BLACK);
  drawHeader("Dyson 空氣品質", health.connection);
  display.setFont(&fonts::efontTW_10);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setCursor(4, 25); display.printf("溫度 %.1f°C   濕度 %u%%", dyson.temperatureC, dyson.humidity);
  display.setCursor(4, 40); display.printf("PM2.5 %u   PM10 %u", dyson.pm25, dyson.pm10);
  display.setCursor(4, 55); display.printf("VOC %u   NO₂ %u   甲醛 %u", dyson.voc, dyson.no2, dyson.formaldehyde);
  display.setCursor(4, 70); display.printf("活性碳 %d%%   HEPA %d%%", dyson.carbonFilter, dyson.hepaFilter);
  display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  display.setCursor(4, 88);
  if (dyson.lastReportMs)
    display.printf("資料更新 %lu 秒前", static_cast<unsigned long>(elapsedSince(millis(), dyson.lastReportMs) / 1000));
  else
    display.print("尚未收到感測資料");
  display.setTextColor(feedbackColor, TFT_BLACK); display.setCursor(4, 106); display.print(lastFeedback);
  display.fillRect(0, 120, 240, 15, TFT_NAVY);
  display.setTextColor(TFT_WHITE, TFT_NAVY); display.setCursor(3, 121); display.print("4空氣品質  TAB下一頁  0快捷頁");
  display.pushSprite(0, 0);
}

void drawDiagnosticsPage(const AdapterHealth& dyson, const AdapterHealth& light, const SystemState& system) {
  auto& display = uiCanvas;
  display.fillScreen(TFT_BLACK);
  drawHeader("系統診斷", system.wifiConnected ? ConnectionState::Online : ConnectionState::Offline);
  display.setFont(&fonts::efontTW_10);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setCursor(4, 24); display.printf("版本 %s  Uptime %lus", kFirmwareVersion,
                                         static_cast<unsigned long>(millis() / 1000));
  display.setCursor(4, 38); display.printf("Wi-Fi %ddBm  IP %s", system.rssi, system.ip);
  display.setCursor(4, 52); display.printf("Heap %u  最低 %u", ESP.getFreeHeap(), ESP.getMinFreeHeap());
  display.setCursor(4, 66); display.printf("Dyson %s  最後 %lus", connectionText(dyson.connection),
                                         dyson.lastSuccessMs ? static_cast<unsigned long>(elapsedSince(millis(), dyson.lastSuccessMs) / 1000) : 0UL);
  display.setCursor(4, 80); display.printf("燈具 %u/%u  %s", light.onlineTargets, light.totalTargets,
                                         connectionText(light.connection));
  display.setTextColor(TFT_ORANGE, TFT_BLACK);
  display.setCursor(4, 94);
  const char* error = dyson.lastError[0] ? dyson.lastError : light.lastError[0] ? light.lastError : "無錯誤";
  display.printf("最近錯誤: %.30s", error);
  display.fillRect(0, 120, 240, 15, TFT_NAVY);
  display.setTextColor(TFT_WHITE, TFT_NAVY); display.setCursor(3, 121); display.print("I診斷  TAB下一頁  0快捷頁");
  display.pushSprite(0, 0);
}

void drawUi() {
  AcState ac; DysonState dyson; LightState light;
  AdapterHealth acHealth, dysonHealth, lightHealth;
  SystemState system;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  ac = sharedAc; dyson = sharedDyson; light = sharedLight;
  acHealth = sharedAcHealth; dysonHealth = sharedDysonHealth; lightHealth = sharedLightHealth; system = sharedSystem;
  xSemaphoreGive(stateMutex);
  if (currentPage == UiPage::Quick) {
    drawQuickPage(ac, dyson, light, dysonHealth, lightHealth, system);
    return;
  }
  if (currentPage == UiPage::DysonAir) {
    drawDysonAirPage(dyson, dysonHealth);
    return;
  }
  if (currentPage == UiPage::Diagnostics) {
    drawDiagnosticsPage(dysonHealth, lightHealth, system);
    return;
  }
  auto& display = uiCanvas;
  display.fillScreen(TFT_BLACK);
  const char* title = selectedDevice == DeviceId::Ac ? "冷氣 RG57A" :
                      selectedDevice == DeviceId::Dyson ? "Dyson TP09" : "所有 Tapo 燈";
  const ConnectionState connection = selectedDevice == DeviceId::Ac ? acHealth.connection :
                                     selectedDevice == DeviceId::Dyson ? dysonHealth.connection : lightHealth.connection;
  drawHeader(title, connection);
  display.setFont(&fonts::efontTW_10);
  display.setTextColor(TFT_LIGHTGREY, TFT_BLACK); display.setCursor(4, 23);
  if (selectedDevice == DeviceId::Ac)
    display.printf("紅外線推定狀態｜%u°C", ac.temperature);
  else if (selectedDevice == DeviceId::Dyson)
    display.printf("%.1f°C  %u%%  PM2.5 %u  PM10 %u", dyson.temperatureC, dyson.humidity, dyson.pm25, dyson.pm10);
  else
    display.printf("自動探索 %u/%u 在線%s", light.onlineDevices, light.totalDevices, light.mixed ? "｜狀態不一致" : "");
  if (selectedDevice == DeviceId::Light) {
    display.fillRect(209, 23, 27, 10, lightPreviewColor(light));
    display.drawRect(208, 22, 29, 12, TFT_LIGHTGREY);
  }

  const uint8_t current = selectedItem[deviceIndex(selectedDevice)];
  const uint8_t count = itemCount(selectedDevice);
  const uint8_t first = current < 4 ? 0 : current - 3;
  display.setFont(&fonts::efontTW_12);
  for (uint8_t row = 0; row < 4 && first + row < count; ++row) {
    const uint8_t item = first + row;
    const int y = 37 + row * 17;
    const bool selected = item == current;
    display.fillRect(2, y, 236, 16, selected ? TFT_DARKCYAN : TFT_BLACK);
    display.setTextColor(selected ? TFT_WHITE : TFT_LIGHTGREY, selected ? TFT_DARKCYAN : TFT_BLACK);
    display.setCursor(5, y + 1); display.print(selected ? "▶ " : "  "); display.print(itemLabel(selectedDevice, item));
    const String value = itemValue(selectedDevice, item, ac, dyson, light);
    display.setCursor(234 - display.textWidth(value), y + 1); display.print(value);
  }
  display.setFont(&fonts::efontTW_10);
  display.setTextColor(feedbackColor, TFT_BLACK); display.setCursor(4, 106); display.print(lastFeedback);
  display.setTextColor(TFT_WHITE, TFT_NAVY); display.fillRect(0, 120, 240, 15, TFT_NAVY);
  display.setCursor(3, 121); display.print("TAB設備  W/S選擇  A/D調整  ENTER套用");
  display.pushSprite(0, 0);
}

int cycleTimer(int current, int direction) {
  static const int values[] = {0, 30, 60, 120};
  int index = 0;
  for (int i = 0; i < 4; ++i) if (values[i] == current) index = i;
  return values[(index + direction + 4) % 4];
}

void activateSelected();

void copyDeviceStates(AcState& ac, DysonState& dyson, LightState& light) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  ac = sharedAc; dyson = sharedDyson; light = sharedLight;
  xSemaphoreGive(stateMutex);
}

void adjustSelected(int direction) {
  AcState ac; DysonState dyson; LightState light;
  copyDeviceStates(ac, dyson, light);
  const uint8_t item = selectedItem[deviceIndex(selectedDevice)];
  if (selectedDevice == DeviceId::Ac) {
    if (item == 1) enqueue(DeviceId::Ac, CommandKind::SetTemperature,
                           constrain(effectiveValue(DeviceId::Ac, CommandKind::SetTemperature, ac.temperature) + direction, 17, 30));
    else if (item == 2) enqueue(DeviceId::Ac, CommandKind::SetAcMode,
                                (effectiveValue(DeviceId::Ac, CommandKind::SetAcMode, static_cast<int>(ac.mode)) + direction + 4) % 4);
    else if (item == 3) enqueue(DeviceId::Ac, CommandKind::SetFanSpeed,
                                (effectiveValue(DeviceId::Ac, CommandKind::SetFanSpeed, static_cast<int>(ac.fan)) + direction + 4) % 4);
    else if (item == 10) enqueue(DeviceId::Ac, CommandKind::SetAcOnTimer,
                                 cycleTimer(effectiveValue(DeviceId::Ac, CommandKind::SetAcOnTimer, ac.onTimerMinutes), direction));
    else if (item == 11) enqueue(DeviceId::Ac, CommandKind::SetAcOffTimer,
                                 cycleTimer(effectiveValue(DeviceId::Ac, CommandKind::SetAcOffTimer, ac.offTimerMinutes), direction));
    else activateSelected();
  } else if (selectedDevice == DeviceId::Dyson) {
    if (item == 1) enqueue(DeviceId::Dyson, CommandKind::SetFanSpeed,
                           constrain(effectiveValue(DeviceId::Dyson, CommandKind::SetFanSpeed, dyson.speed) + direction, 1, 10));
    else if (item == 4) {
      const int shift = direction * 15;
      const int low = effectiveValue(DeviceId::Dyson, CommandKind::SetOscillationAngles, dyson.angleLow);
      const int high = effectiveValue(DeviceId::Dyson, CommandKind::SetOscillationAngles, dyson.angleHigh, 2);
      if (low + shift >= 5 && high + shift <= 355)
        enqueue(DeviceId::Dyson, CommandKind::SetOscillationAngles, low + shift, high + shift);
    } else if (item == 8) enqueue(DeviceId::Dyson, CommandKind::SetSleepTimer,
                                  cycleTimer(effectiveValue(DeviceId::Dyson, CommandKind::SetSleepTimer,
                                                            max(0, static_cast<int>(dyson.sleepTimerMinutes))), direction));
    else activateSelected();
  } else {
    if (item == 1) enqueue(DeviceId::Light, CommandKind::SetBrightness,
                           constrain(effectiveValue(DeviceId::Light, CommandKind::SetBrightness, light.brightness) + direction * 10, 1, 100));
    else if (item == 2) enqueue(DeviceId::Light, CommandKind::SetColorTemperature,
                                constrain(effectiveValue(DeviceId::Light, CommandKind::SetColorTemperature,
                                                         light.colorTemperature ? light.colorTemperature : 4000) + direction * 500, 2500, 6500));
    else if (item == 3) enqueue(DeviceId::Light, CommandKind::SetHsv,
                                (effectiveValue(DeviceId::Light, CommandKind::SetHsv, light.hue) + direction * 30 + 360) % 360,
                                effectiveValue(DeviceId::Light, CommandKind::SetHsv, light.saturation, 2),
                                effectiveValue(DeviceId::Light, CommandKind::SetHsv, light.brightness, 3));
    else if (item == 4) enqueue(DeviceId::Light, CommandKind::SetHsv,
                                effectiveValue(DeviceId::Light, CommandKind::SetHsv, light.hue),
                                constrain(effectiveValue(DeviceId::Light, CommandKind::SetHsv, light.saturation, 2) + direction * 10, 0, 100),
                                effectiveValue(DeviceId::Light, CommandKind::SetHsv, light.brightness, 3));
    else if (item == 5) enqueue(DeviceId::Light, CommandKind::SetLightEffect,
                                (effectiveValue(DeviceId::Light, CommandKind::SetLightEffect, light.effect) + direction + 3) % 3);
    else if (item == 6) enqueue(DeviceId::Light, CommandKind::ApplyLightPreset,
                                (effectiveValue(DeviceId::Light, CommandKind::ApplyLightPreset, light.presetIndex) + direction + 7) % 7);
    else activateSelected();
  }
}

void activateSelected() {
  AcState ac; DysonState dyson; LightState light;
  copyDeviceStates(ac, dyson, light);
  const uint8_t item = selectedItem[deviceIndex(selectedDevice)];
  if (selectedDevice == DeviceId::Ac) {
    static const CommandKind toggles[] = {CommandKind::TogglePower, CommandKind::Refresh, CommandKind::Refresh, CommandKind::Refresh,
      CommandKind::ToggleSwing, CommandKind::ToggleSleep, CommandKind::ToggleTurbo, CommandKind::ToggleEco,
      CommandKind::ToggleClean, CommandKind::ToggleLed};
    if (item == 0) enqueue(DeviceId::Ac, CommandKind::SetPower,
                           !effectiveValue(DeviceId::Ac, CommandKind::SetPower, ac.power));
    else if (item < 10 && item != 1 && item != 2 && item != 3) enqueue(DeviceId::Ac, toggles[item]); else adjustSelected(1);
  } else if (selectedDevice == DeviceId::Dyson) {
    if (item == 0) enqueue(DeviceId::Dyson, CommandKind::SetPower,
                           !effectiveValue(DeviceId::Dyson, CommandKind::SetPower, dyson.power));
    else if (item == 2) enqueue(DeviceId::Dyson, CommandKind::SetDysonAuto,
                                !effectiveValue(DeviceId::Dyson, CommandKind::SetDysonAuto, dyson.autoMode));
    else if (item == 3) enqueue(DeviceId::Dyson, CommandKind::SetOscillation,
                                !effectiveValue(DeviceId::Dyson, CommandKind::SetOscillation, dyson.oscillation));
    else if (item == 5) enqueue(DeviceId::Dyson, CommandKind::SetAirflowFront,
                                !effectiveValue(DeviceId::Dyson, CommandKind::SetAirflowFront, dyson.frontAirflow));
    else if (item == 6) enqueue(DeviceId::Dyson, CommandKind::SetNightMode,
                                !effectiveValue(DeviceId::Dyson, CommandKind::SetNightMode, dyson.nightMode));
    else if (item == 7) enqueue(DeviceId::Dyson, CommandKind::SetContinuousMonitoring,
                                !effectiveValue(DeviceId::Dyson, CommandKind::SetContinuousMonitoring, dyson.continuousMonitoring));
    else adjustSelected(1);
  } else {
    if (item == 0) enqueue(DeviceId::Light, CommandKind::SetPower,
                           light.mixed ? 1 : !effectiveValue(DeviceId::Light, CommandKind::SetPower, light.power));
    else adjustSelected(1);
  }
}

void handleQuickKey(char key) {
  AcState ac; DysonState dyson; LightState light;
  copyDeviceStates(ac, dyson, light);
  switch (key) {
    case 'q': enqueue(DeviceId::Ac, CommandKind::SetPower, !effectiveValue(DeviceId::Ac, CommandKind::SetPower, ac.power)); break;
    case 'w': enqueue(DeviceId::Ac, CommandKind::SetTemperature, max(17, effectiveValue(DeviceId::Ac, CommandKind::SetTemperature, ac.temperature) - 1)); break;
    case 'e': enqueue(DeviceId::Ac, CommandKind::SetTemperature, min(30, effectiveValue(DeviceId::Ac, CommandKind::SetTemperature, ac.temperature) + 1)); break;
    case 'r': enqueue(DeviceId::Ac, CommandKind::SetAcMode, (effectiveValue(DeviceId::Ac, CommandKind::SetAcMode, static_cast<int>(ac.mode)) + 1) % 4); break;
    case 'a': enqueue(DeviceId::Dyson, CommandKind::SetPower, !effectiveValue(DeviceId::Dyson, CommandKind::SetPower, dyson.power)); break;
    case 's': enqueue(DeviceId::Dyson, CommandKind::SetFanSpeed, max(1, effectiveValue(DeviceId::Dyson, CommandKind::SetFanSpeed, dyson.speed) - 1)); break;
    case 'd': enqueue(DeviceId::Dyson, CommandKind::SetFanSpeed, min(10, effectiveValue(DeviceId::Dyson, CommandKind::SetFanSpeed, dyson.speed) + 1)); break;
    case 'f': enqueue(DeviceId::Dyson, CommandKind::SetOscillation, !effectiveValue(DeviceId::Dyson, CommandKind::SetOscillation, dyson.oscillation)); break;
    case 'g':
      if (effectiveValue(DeviceId::Dyson, CommandKind::SetOscillationAngles, dyson.angleLow) >= 20)
        enqueue(DeviceId::Dyson, CommandKind::SetOscillationAngles,
                effectiveValue(DeviceId::Dyson, CommandKind::SetOscillationAngles, dyson.angleLow) - 15,
                effectiveValue(DeviceId::Dyson, CommandKind::SetOscillationAngles, dyson.angleHigh, 2) - 15);
      break;
    case 'h':
      if (effectiveValue(DeviceId::Dyson, CommandKind::SetOscillationAngles, dyson.angleHigh, 2) <= 340)
        enqueue(DeviceId::Dyson, CommandKind::SetOscillationAngles,
                effectiveValue(DeviceId::Dyson, CommandKind::SetOscillationAngles, dyson.angleLow) + 15,
                effectiveValue(DeviceId::Dyson, CommandKind::SetOscillationAngles, dyson.angleHigh, 2) + 15);
      break;
    case 'z': enqueue(DeviceId::Light, CommandKind::SetPower, light.mixed ? 1 : !effectiveValue(DeviceId::Light, CommandKind::SetPower, light.power)); break;
    case 'x': enqueue(DeviceId::Light, CommandKind::SetColorTemperature,
                      effectiveValue(DeviceId::Light, CommandKind::SetColorTemperature, light.colorTemperature) >= 4500 ? 2700 : 6500); break;
    case 'c': enqueue(DeviceId::Light, CommandKind::SetBrightness,
                      max(1, effectiveValue(DeviceId::Light, CommandKind::SetBrightness, light.brightness) - 10)); break;
    case 'v': enqueue(DeviceId::Light, CommandKind::SetBrightness,
                      min(100, effectiveValue(DeviceId::Light, CommandKind::SetBrightness, light.brightness) + 10)); break;
    default: break;
  }
}

void handleKeys() {
  M5Cardputer.update();
  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
  const auto keys = M5Cardputer.Keyboard.keysState();
  if (keys.tab) {
    currentPage = static_cast<UiPage>((static_cast<uint8_t>(currentPage) + 1) % 6);
    if (currentPage == UiPage::Ac) selectedDevice = DeviceId::Ac;
    else if (currentPage == UiPage::Dyson) selectedDevice = DeviceId::Dyson;
    else if (currentPage == UiPage::Light) selectedDevice = DeviceId::Light;
    return;
  }
  if ((currentPage == UiPage::Ac || currentPage == UiPage::Dyson || currentPage == UiPage::Light) && keys.enter)
    activateSelected();
  for (char raw : keys.word) {
    const char key = static_cast<char>(tolower(static_cast<unsigned char>(raw)));
    if (key == '0') { currentPage = UiPage::Quick; continue; }
    if (key >= '1' && key <= '3') {
      selectedDevice = static_cast<DeviceId>(key - '0');
      currentPage = key == '1' ? UiPage::Ac : key == '2' ? UiPage::Dyson : UiPage::Light;
      continue;
    }
    if (key == '4') { currentPage = UiPage::DysonAir; continue; }
    if (key == 'i') { currentPage = UiPage::Diagnostics; continue; }
    if (currentPage == UiPage::Quick) { handleQuickKey(key); continue; }
    if (currentPage == UiPage::DysonAir || currentPage == UiPage::Diagnostics) continue;
    if (key == 'w' || key == 'k') {
      auto& item = selectedItem[deviceIndex(selectedDevice)]; item = (item + itemCount(selectedDevice) - 1) % itemCount(selectedDevice);
    } else if (key == 's' || key == 'j') {
      auto& item = selectedItem[deviceIndex(selectedDevice)]; item = (item + 1) % itemCount(selectedDevice);
    } else if (key == 'a' || key == 'h') adjustSelected(-1);
    else if (key == 'd' || key == 'l') adjustSelected(1);
    else if (key == ' ') {
      AcState ac; DysonState dyson; LightState light;
      copyDeviceStates(ac, dyson, light);
      const bool current = selectedDevice == DeviceId::Ac ?
          effectiveValue(DeviceId::Ac, CommandKind::SetPower, ac.power) :
          selectedDevice == DeviceId::Dyson ? effectiveValue(DeviceId::Dyson, CommandKind::SetPower, dyson.power) :
          effectiveValue(DeviceId::Light, CommandKind::SetPower, light.power);
      enqueue(selectedDevice, CommandKind::SetPower,
              selectedDevice == DeviceId::Light && light.mixed ? 1 : !current);
    }
  }
}

void uiTask(void*) {
  uint32_t nextDraw = 0;
  for (;;) {
    handleKeys();
    DeviceEvent event;
    while (xQueueReceive(eventQueue, &event, 0) == pdTRUE) {
      Serial.printf("event device=%u id=%lu result=%u msg=%s\n", static_cast<unsigned>(event.device),
                    static_cast<unsigned long>(event.commandId), static_cast<unsigned>(event.result), event.message);
      lastDeviceResult[deviceIndex(event.device)] = event.result;
      const char* feedback = event.result == EventResult::Pending ? "設備處理中，等待狀態回報" :
                             event.result == EventResult::Succeeded ? "操作完成" :
                             event.result == EventResult::PartiallySucceeded ? "部分設備完成，請查看診斷" :
                             event.result == EventResult::AuthFailed ? "認證失敗，請重新設定" :
                             event.result == EventResult::Offline ? "設備離線，正在重新探索" :
                             event.result == EventResult::TimedOut ? "操作逾時" : "此功能不支援";
      strncpy(lastFeedback, feedback, sizeof(lastFeedback) - 1);
      if (event.result == EventResult::PartiallySucceeded)
        snprintf(lastFeedback, sizeof(lastFeedback), "部分完成 %u/%u", event.succeededTargets, event.totalTargets);
      lastFeedback[sizeof(lastFeedback) - 1] = '\0';
      feedbackColor = event.result == EventResult::Succeeded ? TFT_GREEN :
                      (event.result == EventResult::Pending || event.result == EventResult::PartiallySucceeded) ? TFT_YELLOW : TFT_RED;
    }
    if (millis() >= nextDraw) { drawUi(); nextDraw = millis() + 250; }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

bool connectWifi(const WifiConfig& wifi, bool keepSetupAp = false) {
  WiFi.mode(keepSetupAp ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(wifi.ssid.c_str(), wifi.password.c_str());
  const uint32_t deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) vTaskDelay(pdMS_TO_TICKS(100));
  return WiFi.status() == WL_CONNECTED;
}

SetupValidationResult setupValidation(bool ok, SetupValidationStage stage, const char* detail) {
  SetupValidationResult result;
  result.ok = ok;
  result.stage = stage;
  strncpy(result.detail, detail ? detail : "", sizeof(result.detail) - 1);
  return result;
}

SetupValidationResult validateSetupCandidate(const AppConfig& candidate) {
  if (!connectWifi(candidate.wifi, true))
    return setupValidation(false, SetupValidationStage::Wifi, "無法連上指定的無線網路");
  if (!tapoAdapter.begin(candidate))
    return setupValidation(false, SetupValidationStage::Tapo, "找不到 L530E 或 KLAP 認證失敗");
  tapoAdapter.tick(millis());
  if (tapoAdapter.connectionState() != ConnectionState::Online ||
      tapoAdapter.snapshot().lastReportMs == 0)
    return setupValidation(false, SetupValidationStage::Tapo, "未能讀取全部已探索燈具的狀態");
  if (!dysonAdapter.begin(candidate))
    return setupValidation(false, SetupValidationStage::Dyson, "mDNS 找不到 TP09 或序號不相符");
  const uint32_t started = millis();
  while (millis() - started < 12000) {
    dysonAdapter.tick(millis());
    if (dysonAdapter.connectionState() == ConnectionState::AuthFailed)
      return setupValidation(false, SetupValidationStage::Dyson, "本地 MQTT credential 驗證失敗");
    if (dysonAdapter.connectionState() == ConnectionState::Online &&
        dysonAdapter.snapshot().lastReportMs != 0)
      return setupValidation(true, SetupValidationStage::None, "驗證完成");
    delay(10);
  }
  return setupValidation(false, SetupValidationStage::Dyson, "MQTT 狀態回報逾時");
}

bool isTerminal(EventResult result) { return result != EventResult::Pending; }

void forwardNetworkEvent(const DeviceEvent& event) {
  if (isTerminal(event.result)) {
    xSemaphoreTake(commandMutex, portMAX_DELAY);
    commandDispatcher.complete(event.commandId);
    xSemaphoreGive(commandMutex);
  }
  xQueueSend(eventQueue, &event, 0);
}

void drainAdapterEvents(DeviceAdapter& adapter) {
  DeviceEvent event;
  while (adapter.pollEvent(event)) forwardNetworkEvent(event);
}

void networkTask(void*) {
  irAdapter.begin(appConfig);
  WiFi.mode(WIFI_STA);
  WiFi.begin(appConfig.wifi.ssid.c_str(), appConfig.wifi.password.c_str());
  uint32_t nextWifiAttemptMs = millis() + 1000;
  uint32_t wifiRetryMs = 1000;
  bool lanStarted = false;
  bool wasConnected = false;
  IPAddress lastIp;
  uint32_t observedAcSendMs = irAdapter.snapshot().lastSentMs;
  uint32_t acSaveDueMs = 0;
  bool acSavePending = false;
  for (;;) {
    const uint32_t now = millis();
    const bool connected = WiFi.status() == WL_CONNECTED;
    if (!connected && deadlineReached(now, nextWifiAttemptMs)) {
      WiFi.begin(appConfig.wifi.ssid.c_str(), appConfig.wifi.password.c_str());
      nextWifiAttemptMs = now + wifiRetryMs;
      wifiRetryMs = nextReconnectDelay(wifiRetryMs);
    }
    if (connected && (!wasConnected || WiFi.localIP() != lastIp)) {
      lastIp = WiFi.localIP();
      wifiRetryMs = 1000;
      nextWifiAttemptMs = 0;
      dysonAdapter.begin(appConfig);
      tapoAdapter.begin(appConfig);
      lanStarted = true;
    }
    if (lanStarted) {
      dysonAdapter.tick(now);
      tapoAdapter.tick(now);
    }
    irAdapter.tick(now);
    DeviceCommand command;
    xSemaphoreTake(commandMutex, portMAX_DELAY);
    const bool haveCommand = commandDispatcher.take(command);
    xSemaphoreGive(commandMutex);
    if (haveCommand) {
      DeviceEvent event;
      if (command.device == DeviceId::Ac) event = irAdapter.execute(command);
      else if (command.device == DeviceId::Dyson) event = dysonAdapter.execute(command);
      else event = tapoAdapter.execute(command);
      forwardNetworkEvent(event);
    }
    drainAdapterEvents(irAdapter);
    drainAdapterEvents(dysonAdapter);
    drainAdapterEvents(tapoAdapter);
    if (irAdapter.snapshot().lastSentMs != observedAcSendMs) {
      observedAcSendMs = irAdapter.snapshot().lastSentMs;
      acSaveDueMs = now + 5000;
      acSavePending = true;
    }
    if (acSavePending && deadlineReached(now, acSaveDueMs)) {
      if (configStore.saveAcState(irAdapter.snapshot())) acSavePending = false;
      else acSaveDueMs = now + 5000;
    }
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    sharedAc = irAdapter.snapshot(); sharedDyson = dysonAdapter.snapshot(); sharedLight = tapoAdapter.snapshot();
    sharedAcHealth = irAdapter.health(); sharedDysonHealth = dysonAdapter.health(); sharedLightHealth = tapoAdapter.health();
    sharedSystem.wifiConnected = connected;
    sharedSystem.rssi = connected ? WiFi.RSSI() : 0;
    strncpy(sharedSystem.ip, connected ? WiFi.localIP().toString().c_str() : "0.0.0.0", sizeof(sharedSystem.ip) - 1);
    sharedSystem.ip[sizeof(sharedSystem.ip) - 1] = '\0';
    xSemaphoreGive(stateMutex);
    wasConnected = connected;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

bool bootSetupRequested() {
  const uint32_t started = millis();
  while (millis() - started < 5000) {
    M5Cardputer.update();
    if (!M5Cardputer.Keyboard.isKeyPressed('`')) return false;
    delay(20);
  }
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setFont(&fonts::efontTW_12);
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.setCursor(8, 8);
  M5Cardputer.Display.print("Cardputer Home Controller");

  const bool configured = configStore.load(appConfig);
  AcState restoredAc;
  if (configStore.loadAcState(restoredAc)) irAdapter.restoreState(restoredAc);
  setupMode = !configured || bootSetupRequested();
  if (setupMode) {
    setupPortal.begin(configStore, appConfig, validateSetupCandidate);
    M5Cardputer.Display.setCursor(8, 35); M5Cardputer.Display.print("AP: Cardputer-Home-Setup");
    M5Cardputer.Display.setCursor(8, 52); M5Cardputer.Display.print("PASS: "); M5Cardputer.Display.print(setupPortal.apPassword());
    M5Cardputer.Display.setCursor(8, 69); M5Cardputer.Display.print("Open http://192.168.4.1");
    return;
  }

  eventQueue = xQueueCreate(24, sizeof(DeviceEvent));
  stateMutex = xSemaphoreCreateMutex();
  commandMutex = xSemaphoreCreateMutex();
  uiCanvas.setColorDepth(8);
  uiCanvas.createSprite(240, 135);
  uiCanvas.setTextWrap(false);
  xTaskCreatePinnedToCore(uiTask, "ui", 6144, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(networkTask, "network", 12288, nullptr, 2, nullptr, 0);
}

void loop() {
  if (setupMode) {
    setupPortal.tick(millis());
    delay(5);
  } else {
    delay(1000);
  }
}
