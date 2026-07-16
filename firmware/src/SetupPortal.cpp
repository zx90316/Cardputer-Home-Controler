#include "SetupPortal.h"

#include <Arduino.h>
#include <WiFi.h>

namespace chc {

namespace {
constexpr uint32_t kPortalTimeoutMs = 10UL * 60UL * 1000UL;
const char kPage[] PROGMEM = R"HTML(<!doctype html><html lang="zh-Hant"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Cardputer 家電設定</title>
<style>body{font:16px sans-serif;max-width:42rem;margin:auto;padding:1rem;background:#111;color:#eee}fieldset{margin:1rem 0;border:1px solid #555}input{box-sizing:border-box;width:100%;padding:.6rem;margin:.3rem 0;background:#222;color:#fff;border:1px solid #666}button{padding:.8rem 1.2rem}</style>
<h1>Cardputer Adv 家電設定</h1><p>所有資料只送到目前 Cardputer AP。</p><form method="post" action="/save">
<fieldset><legend>Wi-Fi</legend><input name="wifi_ssid" placeholder="SSID" maxlength="32" required><input type="password" name="wifi_password" placeholder="Password" maxlength="64"></fieldset>
<fieldset><legend>Tapo L530E 群組</legend><p>會自動探索並控制同網段所有 L530E，不需填 IP。</p><input type="email" name="tapo_user" placeholder="Tapo email" maxlength="254" required><input type="password" name="tapo_password" placeholder="Tapo password" maxlength="128" required></fieldset>
<fieldset><legend>Dyson TP09</legend><p>會以 mDNS 自動取得 IP。</p><input name="dyson_serial" placeholder="Serial" maxlength="32" required><input name="dyson_type" value="438K" minlength="2" maxlength="8" pattern="[A-Za-z0-9]+" required><input type="password" name="dyson_credential" placeholder="Local MQTT credential" maxlength="512" required></fieldset>
<button type="submit">儲存並重新啟動</button></form></html>)HTML";
}

bool SetupPortal::begin(ConfigStore& store, AppConfig& config, Validator validator) {
  store_ = &store;
  config_ = &config;
  validator_ = validator;
  char password[13];
  snprintf(password, sizeof(password), "%08lX%04lX", static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random() & 0xFFFF));
  apPassword_ = password;
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP("Cardputer-Home-Setup", apPassword_.c_str())) return false;
  server_.on("/", HTTP_GET, [this] { handleRoot(); });
  server_.on("/save", HTTP_POST, [this] { handleSave(); });
  server_.onNotFound([this] { noStore(); server_.sendHeader("Location", "/"); server_.send(302); });
  server_.begin();
  startedMs_ = millis();
  active_ = true;
  return true;
}

String SetupPortal::field(const char* name, bool trim) {
  String value = server_.arg(name);
  if (trim) value.trim();
  return value;
}

void SetupPortal::noStore() {
  server_.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server_.sendHeader("Pragma", "no-cache");
}

void SetupPortal::handleRoot() { noStore(); server_.send_P(200, "text/html; charset=utf-8", kPage); }

void SetupPortal::handleSave() {
  AppConfig candidate;
  candidate.wifi.ssid = field("wifi_ssid").c_str();
  candidate.wifi.password = field("wifi_password", false).c_str();
  candidate.tapo.username = field("tapo_user").c_str();
  candidate.tapo.password = field("tapo_password", false).c_str();
  candidate.dyson.serial = field("dyson_serial").c_str();
  candidate.dyson.productType = field("dyson_type").c_str();
  candidate.dyson.credential = field("dyson_credential", false).c_str();
  if (!candidate.structurallyValid()) {
    noStore(); server_.send(400, "text/plain; charset=utf-8", "設定不完整或欄位格式/長度不正確");
    return;
  }
  const SetupValidationResult validation = validator_ ? validator_(candidate) : SetupValidationResult{};
  if (!validation.ok) {
    const char* stage = validation.stage == SetupValidationStage::Wifi ? "Wi-Fi" :
                        validation.stage == SetupValidationStage::Tapo ? "Tapo" :
                        validation.stage == SetupValidationStage::Dyson ? "Dyson" : "設定";
    String message = String(stage) + " 驗證失敗：" + (validation.detail[0] ? validation.detail : "請檢查裝置與憑證");
    noStore(); server_.send(400, "text/plain; charset=utf-8", message);
    return;
  }
  if (!store_->save(candidate)) {
    noStore(); server_.send(500, "text/plain; charset=utf-8", "連線成功，但 NVS 寫入失敗");
    return;
  }
  *config_ = candidate;
  noStore(); server_.send(200, "text/plain; charset=utf-8", "設定已儲存，Cardputer 即將重新啟動");
  delay(500);
  ESP.restart();
}

void SetupPortal::tick(uint32_t nowMs) {
  if (!active_) return;
  server_.handleClient();
  if (nowMs - startedMs_ >= kPortalTimeoutMs) {
    server_.stop();
    WiFi.softAPdisconnect(true);
    active_ = false;
    delay(50);
    ESP.restart();
  }
}

}  // namespace chc
