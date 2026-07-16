# 支援

[English](SUPPORT.md) · 繁體中文

## 回報位置

- **已確認 Bug：**使用 [bug report 表單](https://github.com/zx90316/Cardputer-Home-Controler/issues/new?template=bug_report.yml)。
- **功能提案：**使用 [feature request 表單](https://github.com/zx90316/Cardputer-Home-Controler/issues/new?template=feature_request.yml)。
- **新硬體證據：**使用 hardware compatibility 表單。
- **安全問題：**禁止建立公開 issue，請依 [SECURITY.md](SECURITY.md) 回報。

這是社群專案，支援採 best effort，不承諾回覆時間或設備相容性。

## 建立 issue 前

1. 確認控制器與設備位於同一個可互通 IPv4 LAN。
2. 確認 guest network、VLAN 或 client isolation 沒有阻擋 UDP broadcast／mDNS。
3. 升級至最新 release candidate。
4. 重新啟動 Cardputer 與受影響家電後再次重現。
5. 執行唯讀 probe 或相應 local probe。
6. 查看診斷頁與 Serial output，僅保留遮蔽後的錯誤。
7. 搜尋是否已有相同 device firmware 與症狀的 issue。

## 有助於診斷的資訊

- Cardputer firmware version 與 SHA-256 manifest；
- 目標 model、hardware revision、product type 與 firmware version；
- router/AP 型號，以及是否使用 guest Wi-Fi、mesh、VLAN 或 client isolation；
- 實際按鍵或頁面操作順序；
- 預期與實際結果；
- 發生頻率與時間；
- 遮蔽後的 diagnostics 或 Serial log；
- `python probe.py ...` 結果與 stage/code；
- WAN 是否可用，以及 LAN 控制是否仍正常。

UI 問題可附照片，但請先確認畫面沒有設定密碼、IP、serial 或 credential。

## 禁止公開

- `.secrets/dyson-local.json`；
- Wi-Fi、Tapo 或 MyDyson 密碼；
- MyDyson OTP；
- TP09 本地 MQTT credential；
- 未遮蔽的設定頁、HTTP payload、cookie 或 packet capture；
- 其他人的私人資料。

## 常見分類

| 症狀 | 優先檢查 |
| --- | --- |
| 找不到 L530E | 同一 LAN、UDP broadcast、Tapo App 配網、guest isolation |
| TP09 離線 | Product type `438K`、本地 credential、mDNS、單一 MQTT client |
| IR 無反應 | 發射方向／距離、GPIO 44、RG57A 尾碼、推定狀態、`IR last/max ms` |
| IR 延遲 | 使用 RC3 以上，檢查 Serial `latency_ms` 是否有 `WARNING` |
| 螢幕不會喚醒 | 診斷頁必須顯示 `IMU ON`，測試移動與第一鍵只喚醒 |
| 設定消失 | 檢查 schema validation 與雙槽交易式 NVS |

## 範圍邊界

需要 Home Assistant、always-on server、遠端網際網路控制、OTA、場景、排程或跨 VLAN routing 的需求，因不符合目前 local-only 方向，可能不會接受。
