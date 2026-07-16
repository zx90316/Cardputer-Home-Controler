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
#include "core/PowerPolicy.h"
#include "core/ProtocolHelpers.h"
#include "core/StateReducer.h"
#include "core/TimeUtils.h"

using namespace chc;

namespace {

QueueHandle_t eventQueue;
SemaphoreHandle_t stateMutex;
SemaphoreHandle_t commandMutex;
CommandDispatcher<16> irCommandDispatcher;
CommandDispatcher<24> networkCommandDispatcher;
TaskHandle_t irTaskHandle{nullptr};
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
  uint32_t lastIrLatencyMs{0};
  uint32_t maxIrLatencyMs{0};
} sharedSystem;
uint32_t acStateRevision{0};
uint32_t acSaveDueMs{0};
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
constexpr const char* kFirmwareVersion = "1.0.0-rc3";
constexpr uint32_t kIrLatencyTargetMs = 50;

struct UiRuntime {
  int32_t batteryLevel{-1};
  bool charging{false};
  bool imuAvailable{false};
  bool dimmed{false};
  bool sleeping{false};
  bool haveMotionSample{false};
  bool forceRedraw{true};
  float lastAx{0};
  float lastAy{0};
  float lastAz{0};
  uint32_t lastActivityMs{0};
  uint32_t nextMotionSampleMs{0};
  uint32_t nextBatterySampleMs{0};
} uiRuntime;

constexpr uint8_t kNormalBrightness = 150;
constexpr uint8_t kDimBrightness = 22;
constexpr uint32_t kDimAfterMs = 30000;
constexpr uint32_t kSleepAfterMs = 120000;
constexpr uint16_t kUiBackground = 0x0863;
constexpr uint16_t kUiSurface = 0x10C5;
constexpr uint16_t kUiRaised = 0x1928;
constexpr uint16_t kUiBorder = 0x29CB;
constexpr uint16_t kUiMuted = 0x9CF3;
constexpr uint16_t kUiBlue = 0x2D7F;
constexpr uint16_t kUiCyan = 0x4E7F;
constexpr uint16_t kUiAmber = 0xFD20;
constexpr uint16_t kUiGreen = 0x4E67;
constexpr uint16_t kUiRed = 0xF9E7;

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
  DeviceCommand command{device, kind, v1, v2, v3, nextCommandId++, millis()};
  xSemaphoreTake(commandMutex, portMAX_DELAY);
  const bool accepted = device == DeviceId::Ac ? irCommandDispatcher.push(command) :
                                                 networkCommandDispatcher.push(command);
  xSemaphoreGive(commandMutex);
  if (accepted && device == DeviceId::Ac && irTaskHandle) xTaskNotifyGive(irTaskHandle);
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
  const bool found = device == DeviceId::Ac ? irCommandDispatcher.latest(device, kind, command) :
                                              networkCommandDispatcher.latest(device, kind, command);
  xSemaphoreGive(commandMutex);
  return found;
}

int effectiveValue(DeviceId device, CommandKind kind, int fallback, uint8_t field = 1) {
  DeviceCommand command;
  if (!latestCommand(device, kind, command)) return fallback;
  return field == 2 ? command.value2 : field == 3 ? command.value3 : command.value1;
}

bool effectiveLightGroupPowerTarget(const LightState& light) {
  DeviceCommand pending;
  const bool hasPending = latestCommand(DeviceId::Light, CommandKind::SetPower, pending);
  return nextLightGroupPowerTarget(light, hasPending, hasPending && pending.value1 != 0);
}

uint8_t deviceIndex(DeviceId device) { return static_cast<uint8_t>(device) - 1; }
uint8_t itemCount(DeviceId device) {
  return device == DeviceId::Ac ? 12 : device == DeviceId::Dyson ? 9 : 7;
}
const char* onOff(bool value) { return value ? "開" : "關"; }
String timerText(int minutes) { return minutes > 0 ? String(minutes) + " 分" : "關閉"; }

String mixedLightText(const LightState& light) {
  if (!light.mixed) return "";
  String value = "異:";
  if (light.mixedFields & 0x01) value += "電";
  if (light.mixedFields & 0x02) value += "亮";
  if (light.mixedFields & 0x04) value += "色";
  return value;
}

String mixedValue(const String& value, bool mixed) {
  return mixed ? String("混合·") + value : value;
}

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
    case 1: return mixedValue(String(light.brightness) + "%", light.mixedFields & 0x02);
    case 2: return mixedValue(light.colorTemperature ? String(light.colorTemperature) + " K" : "彩色模式",
                              light.mixedFields & 0x04);
    case 3: return mixedValue(String(light.hue) + "°", light.mixedFields & 0x04);
    case 4: return mixedValue(String(light.saturation) + "%", light.mixedFields & 0x04);
    case 5: return mixedValue(light.effect == 1 ? "Party" : light.effect == 2 ? "Relax" : "關閉",
                              light.mixedFields & 0x04);
    default: return mixedValue(light.presetIndex >= 0 ? String(light.presetIndex + 1) : "未選",
                               light.mixedFields & 0x04);
  }
}

uint16_t connectionColor(ConnectionState state) {
  if (state == ConnectionState::Online) return kUiGreen;
  if (state == ConnectionState::Degraded || state == ConnectionState::Connecting) return kUiAmber;
  return kUiRed;
}

const char* headerStatusText(ConnectionState state) {
  switch (state) {
    case ConnectionState::Online: return "ON";
    case ConnectionState::Degraded: return "PART";
    case ConnectionState::Connecting: return "LINK";
    case ConnectionState::AuthFailed: return "AUTH";
    case ConnectionState::Offline: return "OFF";
    default: return "CFG";
  }
}

void drawBattery() {
  auto& display = uiCanvas;
  const int x = 215;
  const int y = 6;
  display.drawRoundRect(x, y, 19, 9, 2, kUiMuted);
  display.fillRect(x + 19, y + 3, 2, 3, kUiMuted);
  if (uiRuntime.batteryLevel > 0) {
    const int fill = constrain(uiRuntime.batteryLevel * 15 / 100, 1, 15);
    const uint16_t color = uiRuntime.batteryLevel <= 15 ? kUiRed :
                           uiRuntime.batteryLevel <= 35 ? kUiAmber : kUiGreen;
    display.fillRoundRect(x + 2, y + 2, fill, 5, 1, color);
  }
  display.setFont(&fonts::Font0);
  display.setTextColor(uiRuntime.charging ? kUiAmber : kUiMuted, kUiBackground);
  display.setCursor(185, 7);
  if (uiRuntime.batteryLevel >= 0) display.printf("%s%ld%%", uiRuntime.charging ? "+" : "", static_cast<long>(uiRuntime.batteryLevel));
  else display.print("--%");
}

void drawHeader(const char* title, ConnectionState state) {
  auto& display = uiCanvas;
  display.fillRect(0, 0, 240, 22, kUiBackground);
  display.fillRoundRect(3, 5, 3, 12, 1, kUiBlue);
  display.setFont(&fonts::efontTW_10);
  display.setTextColor(TFT_WHITE, kUiBackground);
  display.setCursor(10, 5); display.print(title);
  const uint16_t statusColor = connectionColor(state);
  display.fillCircle(151, 10, 3, statusColor);
  display.setFont(&fonts::Font0);
  display.setTextColor(kUiMuted, kUiBackground);
  display.setCursor(157, 7); display.print(headerStatusText(state));
  drawBattery();
}

void drawCard(int x, int y, int width, int height, uint16_t accent, bool selected = false) {
  auto& display = uiCanvas;
  display.fillRoundRect(x, y, width, height, 5, selected ? kUiRaised : kUiSurface);
  display.drawRoundRect(x, y, width, height, 5, selected ? accent : kUiBorder);
  display.fillRoundRect(x, y, 3, height, 2, accent);
}

void drawFooter(const char* hint) {
  auto& display = uiCanvas;
  display.fillRect(0, 111, 240, 24, kUiRaised);
  display.drawFastHLine(0, 111, 240, kUiBorder);
  display.setFont(&fonts::efontTW_10);
  display.setTextColor(feedbackColor, kUiRaised);
  display.setCursor(5, 112); display.print(lastFeedback);
  display.setFont(&fonts::Font0);
  display.setTextColor(kUiMuted, kUiRaised);
  const int x = max(4, 236 - display.textWidth(hint));
  display.setCursor(x, 126); display.print(hint);
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
  display.fillScreen(kUiBackground);
  const ConnectionState connection = !system.wifiConnected ? ConnectionState::Offline :
      (dysonHealth.connection == ConnectionState::Online && lightHealth.connection == ConnectionState::Online) ? ConnectionState::Online :
      (dysonHealth.connection == ConnectionState::Online || lightHealth.connection == ConnectionState::Online) ? ConnectionState::Degraded :
      ConnectionState::Offline;
  drawHeader("HOME  快速控制", connection);

  drawCard(3, 25, 75, 82, kUiCyan);
  display.setFont(&fonts::efontTW_10);
  display.setTextColor(kUiCyan, kUiSurface); display.setCursor(9, 29);
  display.printf("%s 冷氣", resultIcon(lastDeviceResult[0]));
  display.setFont(&fonts::Font4);
  display.setTextColor(TFT_WHITE, kUiSurface); display.setCursor(10, 44); display.printf("%u", ac.temperature);
  display.setFont(&fonts::efontTW_10); display.setCursor(49, 51); display.print("°C");
  display.setTextColor(ac.power ? kUiGreen : kUiMuted, kUiSurface); display.setCursor(10, 72);
  display.printf("%s · %s", ac.power ? "ON" : "OFF", acModeText(ac.mode));
  display.setFont(&fonts::Font0); display.setTextColor(kUiMuted, kUiSurface);
  display.setCursor(10, 92); display.print("Q  W/E  R");

  drawCard(82, 25, 75, 82, kUiAmber);
  display.setFont(&fonts::efontTW_10); display.setTextColor(kUiAmber, kUiSurface); display.setCursor(88, 29);
  display.printf("%s 風扇", resultIcon(lastDeviceResult[1]));
  display.setFont(&fonts::Font4); display.setTextColor(TFT_WHITE, kUiSurface); display.setCursor(91, 44);
  display.printf("%u", dyson.speed);
  display.setFont(&fonts::efontTW_10); display.setCursor(124, 51); display.print("級");
  display.setTextColor(dyson.power ? kUiGreen : kUiMuted, kUiSurface); display.setCursor(89, 72);
  display.printf("%s · %s", dyson.power ? "ON" : "OFF", dyson.oscillation ? "擺動" : "固定");
  display.setFont(&fonts::Font0); display.setTextColor(kUiMuted, kUiSurface);
  display.setCursor(89, 92); display.print("A S/D F G/H");

  drawCard(161, 25, 76, 82, lightPreviewColor(light));
  display.setFont(&fonts::efontTW_10); display.setTextColor(kUiGreen, kUiSurface); display.setCursor(167, 29);
  display.printf("%s 燈具%s", resultIcon(lastDeviceResult[2]), light.mixed ? "·MIX" : "");
  display.setFont(&fonts::Font4); display.setTextColor(TFT_WHITE, kUiSurface); display.setCursor(168, 44);
  display.printf("%u", light.brightness);
  display.setFont(&fonts::efontTW_10); display.setCursor(225, 51); display.print("%");
  display.setTextColor(light.poweredOnDevices ? kUiGreen : kUiMuted, kUiSurface); display.setCursor(168, 72);
  display.printf("%u/%u 開啟", light.poweredOnDevices, light.onlineDevices);
  display.setFont(&fonts::Font0); display.setTextColor(kUiMuted, kUiSurface);
  display.setCursor(168, 92); display.print("Z X C/V");

  drawFooter("TAB DETAILS  ·  I STATUS");
  display.pushSprite(0, 0);
}

void drawDysonAirPage(const DysonState& dyson, const AdapterHealth& health) {
  auto& display = uiCanvas;
  display.fillScreen(kUiBackground);
  drawHeader("AIR  空氣品質", health.connection);
  const struct Metric { int x; int y; const char* label; String value; uint16_t color; } metrics[] = {
    {3, 25, "溫度", String(dyson.temperatureC, 1) + "°C", kUiCyan},
    {82, 25, "濕度", String(dyson.humidity) + "%", kUiBlue},
    {161, 25, "PM2.5", String(dyson.pm25), kUiGreen},
    {3, 66, "PM10", String(dyson.pm10), kUiGreen},
    {82, 66, "VOC / NO₂", String(dyson.voc) + " / " + String(dyson.no2), kUiAmber},
    {161, 66, "甲醛", String(dyson.formaldehyde), kUiAmber},
  };
  for (const auto& metric : metrics) {
    drawCard(metric.x, metric.y, 75, 37, metric.color);
    display.setFont(&fonts::Font0); display.setTextColor(kUiMuted, kUiSurface);
    display.setCursor(metric.x + 7, metric.y + 5); display.print(metric.label);
    if (metric.value.length() > 8) display.setFont(&fonts::Font0);
    else display.setFont(&fonts::efontTW_12_b);
    display.setTextColor(TFT_WHITE, kUiSurface);
    display.setCursor(metric.x + 7, metric.y + 18); display.print(metric.value);
  }
  display.setFont(&fonts::Font0); display.setTextColor(kUiMuted, kUiBackground); display.setCursor(6, 103);
  display.printf("FILTER C %d%%  H %d%%  ·  %lus", dyson.carbonFilter, dyson.hepaFilter,
                 dyson.lastReportMs ? static_cast<unsigned long>(elapsedSince(millis(), dyson.lastReportMs) / 1000) : 0UL);
  drawFooter("4 AIR  ·  0 HOME");
  display.pushSprite(0, 0);
}

void drawDiagnosticsPage(const AdapterHealth& dyson, const AdapterHealth& light, const SystemState& system) {
  auto& display = uiCanvas;
  display.fillScreen(kUiBackground);
  drawHeader("STATUS  系統診斷", system.wifiConnected ? ConnectionState::Online : ConnectionState::Offline);
  drawCard(3, 25, 114, 37, kUiBlue);
  drawCard(122, 25, 115, 37, kUiGreen);
  drawCard(3, 66, 114, 37, kUiAmber);
  drawCard(122, 66, 115, 37, kUiCyan);
  display.setFont(&fonts::Font0); display.setTextColor(kUiMuted, kUiSurface);
  display.setCursor(10, 30); display.printf("NETWORK %ddBm", system.rssi);
  display.setCursor(129, 30); display.printf("POWER %s", uiRuntime.dimmed ? "SAVE" : "ACTIVE");
  display.setCursor(10, 71); display.print("DEVICES");
  display.setCursor(129, 71); display.print("MEMORY");
  display.setFont(&fonts::Font0); display.setTextColor(TFT_WHITE, kUiSurface);
  display.setCursor(10, 45); display.print(system.ip);
  display.setFont(&fonts::efontTW_10);
  display.setCursor(129, 43);
  if (uiRuntime.batteryLevel >= 0) display.printf("%ld%% ", static_cast<long>(uiRuntime.batteryLevel));
  else display.print("--% ");
  display.printf("%s · IMU %s", uiRuntime.charging ? "充電" : "電池", uiRuntime.imuAvailable ? "ON" : "OFF");
  display.setCursor(10, 84); display.printf("D:%s  L:%u/%u", connectionText(dyson.connection),
                                           light.onlineTargets, light.totalTargets);
  display.setCursor(129, 84); display.printf("%u / %u", ESP.getFreeHeap(), ESP.getMinFreeHeap());
  display.setFont(&fonts::Font0); display.setTextColor(kUiMuted, kUiBackground); display.setCursor(5, 103);
  display.printf("%s · UP %lus · IR %lu/%lums", kFirmwareVersion,
                 static_cast<unsigned long>(millis() / 1000),
                 static_cast<unsigned long>(system.lastIrLatencyMs),
                 static_cast<unsigned long>(system.maxIrLatencyMs));
  const char* error = dyson.lastError[0] ? dyson.lastError : light.lastError[0] ? light.lastError : "無錯誤";
  if (error[0] && strcmp(error, "無錯誤") != 0) {
    display.fillRect(3, 101, 234, 10, kUiBackground);
    display.setTextColor(kUiRed, kUiBackground); display.setCursor(5, 103); display.printf("ERR %.32s", error);
  }
  drawFooter("I STATUS  ·  0 HOME");
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
  display.fillScreen(kUiBackground);
  const char* title = selectedDevice == DeviceId::Ac ? "冷氣 RG57A" :
                      selectedDevice == DeviceId::Dyson ? "Dyson TP09" : "所有 Tapo 燈";
  const ConnectionState connection = selectedDevice == DeviceId::Ac ? acHealth.connection :
                                     selectedDevice == DeviceId::Dyson ? dysonHealth.connection : lightHealth.connection;
  drawHeader(title, connection);
  display.setFont(&fonts::efontTW_10);
  drawCard(3, 25, 234, 16, selectedDevice == DeviceId::Ac ? kUiCyan :
           selectedDevice == DeviceId::Dyson ? kUiAmber : kUiGreen);
  display.setTextColor(kUiMuted, kUiSurface); display.setCursor(9, 27);
  if (selectedDevice == DeviceId::Ac)
    display.printf("IR 推定狀態  ·  %s  %u°C", ac.power ? "ON" : "OFF", ac.temperature);
  else if (selectedDevice == DeviceId::Dyson)
    display.printf("%.1f°C  ·  %u%%  ·  PM2.5 %u", dyson.temperatureC, dyson.humidity, dyson.pm25);
  else
    display.printf("%u/%u在線 · %u開啟 %s", light.onlineDevices, light.totalDevices,
                   light.poweredOnDevices, mixedLightText(light).c_str());
  if (selectedDevice == DeviceId::Light) {
    display.fillRoundRect(212, 28, 20, 9, 2, lightPreviewColor(light));
  }

  const uint8_t current = selectedItem[deviceIndex(selectedDevice)];
  const uint8_t count = itemCount(selectedDevice);
  const uint8_t first = current < 4 ? 0 : current - 3;
  display.setFont(&fonts::efontTW_12);
  for (uint8_t row = 0; row < 4 && first + row < count; ++row) {
    const uint8_t item = first + row;
    const int y = 44 + row * 16;
    const bool selected = item == current;
    const uint16_t accent = selectedDevice == DeviceId::Ac ? kUiCyan : selectedDevice == DeviceId::Dyson ? kUiAmber : kUiGreen;
    display.fillRoundRect(3, y, 234, 14, 3, selected ? kUiRaised : kUiBackground);
    if (selected) display.fillRoundRect(4, y + 2, 3, 10, 1, accent);
    display.setTextColor(selected ? TFT_WHITE : kUiMuted, selected ? kUiRaised : kUiBackground);
    display.setCursor(10, y); display.print(itemLabel(selectedDevice, item));
    const String value = itemValue(selectedDevice, item, ac, dyson, light);
    display.setTextColor(selected ? accent : TFT_WHITE, selected ? kUiRaised : kUiBackground);
    display.setCursor(232 - display.textWidth(value), y); display.print(value);
  }
  drawFooter("W/S SELECT  ·  A/D CHANGE  ·  ENTER");
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
                           effectiveLightGroupPowerTarget(light));
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
    case 'z': {
      enqueue(DeviceId::Light, CommandKind::SetPower,
              effectiveLightGroupPowerTarget(light));
      break;
    }
    case 'x': enqueue(DeviceId::Light, CommandKind::SetColorTemperature,
                      effectiveValue(DeviceId::Light, CommandKind::SetColorTemperature, light.colorTemperature) >= 4500 ? 2700 : 6500); break;
    case 'c': enqueue(DeviceId::Light, CommandKind::SetBrightness,
                      max(1, effectiveValue(DeviceId::Light, CommandKind::SetBrightness, light.brightness) - 10)); break;
    case 'v': enqueue(DeviceId::Light, CommandKind::SetBrightness,
                      min(100, effectiveValue(DeviceId::Light, CommandKind::SetBrightness, light.brightness) + 10)); break;
    default: break;
  }
}

void wakeDisplay(uint32_t now, const char* source) {
  const bool wasPowerSaving = uiRuntime.sleeping || uiRuntime.dimmed;
  if (uiRuntime.sleeping) M5Cardputer.Display.wakeup();
  M5Cardputer.Display.setBrightness(kNormalBrightness);
  uiRuntime.sleeping = false;
  uiRuntime.dimmed = false;
  uiRuntime.lastActivityMs = now;
  uiRuntime.forceRedraw = true;
  if (wasPowerSaving) Serial.printf("ui power=awake source=%s\n", source);
}

void tickUiPower(uint32_t now) {
  if (!uiRuntime.nextBatterySampleMs || deadlineReached(now, uiRuntime.nextBatterySampleMs)) {
    const int32_t level = M5.Power.getBatteryLevel();
    uiRuntime.batteryLevel = level < 0 ? -1 : constrain(level, 0L, 100L);
    uiRuntime.charging = M5.Power.isCharging() == m5::Power_Class::is_charging;
    uiRuntime.nextBatterySampleMs = now + 5000;
  }

  if (uiRuntime.imuAvailable &&
      (!uiRuntime.nextMotionSampleMs || deadlineReached(now, uiRuntime.nextMotionSampleMs))) {
    float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
    if (M5.Imu.getAccel(&ax, &ay, &az) && M5.Imu.getGyro(&gx, &gy, &gz)) {
      if (uiRuntime.haveMotionSample &&
          motionDetected(ax, ay, az, uiRuntime.lastAx, uiRuntime.lastAy, uiRuntime.lastAz, gx, gy, gz))
        wakeDisplay(now, "motion");
      uiRuntime.lastAx = ax;
      uiRuntime.lastAy = ay;
      uiRuntime.lastAz = az;
      uiRuntime.haveMotionSample = true;
    }
    uiRuntime.nextMotionSampleMs = now + 100;
  }

  const ScreenPowerMode mode = screenPowerMode(now, uiRuntime.lastActivityMs, kDimAfterMs, kSleepAfterMs);
  if (mode == ScreenPowerMode::Sleeping && !uiRuntime.sleeping) {
    M5Cardputer.Display.sleep();
    uiRuntime.sleeping = true;
    uiRuntime.dimmed = true;
    Serial.println("ui power=sleeping");
  } else if (mode == ScreenPowerMode::Dimmed && !uiRuntime.dimmed) {
    M5Cardputer.Display.setBrightness(kDimBrightness);
    uiRuntime.dimmed = true;
    uiRuntime.forceRedraw = true;
    Serial.println("ui power=dimmed");
  }
}

void handleKeys() {
  M5Cardputer.update();
  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
  if (uiRuntime.sleeping || uiRuntime.dimmed) {
    wakeDisplay(millis(), "keyboard");
    return;
  }
  uiRuntime.lastActivityMs = millis();
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
              selectedDevice == DeviceId::Light ? effectiveLightGroupPowerTarget(light) : !current);
    }
  }
}

void uiTask(void*) {
  uint32_t nextDraw = 0;
  for (;;) {
    const uint32_t now = millis();
    tickUiPower(now);
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
    if (!uiRuntime.sleeping && (uiRuntime.forceRedraw || !nextDraw || deadlineReached(now, nextDraw))) {
      drawUi();
      uiRuntime.forceRedraw = false;
      nextDraw = now + 250;
    }
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
  result.detail[sizeof(result.detail) - 1] = '\0';
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

void forwardAdapterEvent(const DeviceEvent& event) {
  if (isTerminal(event.result)) {
    xSemaphoreTake(commandMutex, portMAX_DELAY);
    if (event.device == DeviceId::Ac) irCommandDispatcher.complete(event.commandId);
    else networkCommandDispatcher.complete(event.commandId);
    xSemaphoreGive(commandMutex);
  }
  xQueueSend(eventQueue, &event, 0);
}

void drainAdapterEvents(DeviceAdapter& adapter) {
  DeviceEvent event;
  while (adapter.pollEvent(event)) forwardAdapterEvent(event);
}

void irTask(void*) {
  irAdapter.begin(appConfig);
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  sharedAc = irAdapter.snapshot();
  sharedAcHealth = irAdapter.health();
  xSemaphoreGive(stateMutex);

  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    DeviceCommand command;
    for (;;) {
      xSemaphoreTake(commandMutex, portMAX_DELAY);
      const bool haveCommand = irCommandDispatcher.take(command);
      xSemaphoreGive(commandMutex);
      if (!haveCommand) break;

      const uint32_t dispatchMs = millis();
      const uint32_t latencyMs = command.queuedAtMs ? elapsedSince(dispatchMs, command.queuedAtMs) : 0;
      const uint32_t previousSendMs = irAdapter.snapshot().lastSentMs;
      const DeviceEvent event = irAdapter.execute(command);
      Serial.printf("ir dispatch id=%lu latency_ms=%lu target_ms=%lu%s\n",
                    static_cast<unsigned long>(command.id), static_cast<unsigned long>(latencyMs),
                    static_cast<unsigned long>(kIrLatencyTargetMs),
                    latencyMs <= kIrLatencyTargetMs ? "" : " WARNING");
      forwardAdapterEvent(event);
      drainAdapterEvents(irAdapter);

      xSemaphoreTake(stateMutex, portMAX_DELAY);
      sharedAc = irAdapter.snapshot();
      sharedAcHealth = irAdapter.health();
      sharedSystem.lastIrLatencyMs = latencyMs;
      sharedSystem.maxIrLatencyMs = max(sharedSystem.maxIrLatencyMs, latencyMs);
      if (sharedAc.lastSentMs != previousSendMs) {
        ++acStateRevision;
        acSaveDueMs = millis() + 5000;
      }
      xSemaphoreGive(stateMutex);
    }
  }
}

void networkTask(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(appConfig.wifi.ssid.c_str(), appConfig.wifi.password.c_str());
  uint32_t nextWifiAttemptMs = millis() + 1000;
  uint32_t wifiRetryMs = 1000;
  bool lanStarted = false;
  bool wasConnected = false;
  IPAddress lastIp;
  uint32_t persistedAcRevision = 0;
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
    DeviceCommand command;
    xSemaphoreTake(commandMutex, portMAX_DELAY);
    const bool haveCommand = networkCommandDispatcher.take(command);
    xSemaphoreGive(commandMutex);
    if (haveCommand) {
      const DeviceEvent event = command.device == DeviceId::Dyson ? dysonAdapter.execute(command) :
                                                                    tapoAdapter.execute(command);
      forwardAdapterEvent(event);
    }
    drainAdapterEvents(dysonAdapter);
    drainAdapterEvents(tapoAdapter);

    AcState acToPersist;
    uint32_t revision = 0;
    uint32_t saveDue = 0;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    acToPersist = sharedAc;
    revision = acStateRevision;
    saveDue = acSaveDueMs;
    xSemaphoreGive(stateMutex);
    if (revision != persistedAcRevision && deadlineReached(now, saveDue)) {
      if (configStore.saveAcState(acToPersist)) persistedAcRevision = revision;
    }
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    sharedDyson = dysonAdapter.snapshot(); sharedLight = tapoAdapter.snapshot();
    sharedDysonHealth = dysonAdapter.health(); sharedLightHealth = tapoAdapter.health();
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
  cfg.internal_imu = true;
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(kNormalBrightness);
  uiRuntime.imuAvailable = M5.Imu.isEnabled();
  uiRuntime.lastActivityMs = millis();
  Serial.printf("ui imu=%s\n", uiRuntime.imuAvailable ? "enabled" : "unavailable");
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
  xTaskCreatePinnedToCore(irTask, "ir", 4096, nullptr, 3, &irTaskHandle, 1);
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
