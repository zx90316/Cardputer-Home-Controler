# RG57A / Midea 48-bit 預備映射

Python 階段沒有 IR 硬體，因此本文件只固定第一次 Cardputer 實機測試的預備映射，不把它視為 RG57A 已驗證的碼型。

| 功能 | `IRMideaAC` 預備映射 | 範圍 / 備註 |
|---|---|---|
| 電源 | `setPower` | On / Off |
| 溫度 | `setTemp` | 17–30 °C |
| 模式 | `setMode` | Auto / Cool / Dry / Fan；Heat 預設停用 |
| 風速 | `setFan` | Auto / Low / Med / High |
| 垂直擺風 | `setSwingVToggle` | 切換垂直擺風 |
| 睡眠 | `setSleep` | On / Off |
| Turbo | `setTurbo` | On / Off |
| Eco | `setEconoToggle` | 切換 |
| Clean | `setCleanToggle` | 切換 |
| LED | `setLightToggle` | 切換 |
| 開/關機計時 | `setOnTimer` / `setOffTimer` | 硬體階段確認遺留狀態行為 |

Follow Me 不實作；Cardputer Adv 沒有可以代替遙控器室溫感測的合適感測器。如果第一次硬體發射失敗，先取得遙控器完整尾碼或使用外接 IR receiver 錄碼，不擴充 UI。

`IRMideaAC` 預設使用華氏溫標，因此韌體在每次建立 48-bit 狀態時都會明確呼叫 `setUseCelsius(true)`，並以攝氏呼叫 `setTemp(..., true)`。若省略此設定，17–30 的 UI 數值會被當成華氏編碼，冷氣面板可能出現 62 或 8x 等不正常數值。

Swing、Turbo、Eco、Clean 與 LED 並非可直接寫入的持續狀態，而是額外送出的 Midea one-shot toggle frame。韌體在開啟與關閉兩次轉換都固定送出 `true` pulse；不能把 UI 的目標布林值直接傳給 `set*Toggle()`。
