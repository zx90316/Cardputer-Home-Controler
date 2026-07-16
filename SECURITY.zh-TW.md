# 安全政策

[English](SECURITY.md) · 繁體中文

## 支援版本

安全修正以最新 release candidate 與 default branch 為目標。

| 版本 | 支援狀態 |
| --- | --- |
| `1.0.0-rc3` | 支援 |
| 更早的 release candidate | 不支援；回報前請先升級 |
| 尚未發布的 default branch | Best effort |

## 回報安全問題

禁止在公開 issue 揭露漏洞、credential、設備 serial、私人 IP、含敏感資料的 packet capture 或可直接利用的 exploit 細節。

建議回報方式：

1. 開啟此 repository 的 **Security** 分頁；若有 **Report a vulnerability**，請使用 GitHub private vulnerability reporting。
2. 若沒有該功能，請透過 [maintainer GitHub profile](https://github.com/zx90316) 公開的聯絡方式私下聯絡 owner。

只提供重現所需的最少資訊：

- 受影響版本或 commit；
- 受影響設備與 firmware version；
- 影響與實際攻擊前提；
- 可重現步驟或最小 proof of concept；
- 是否需要實體、LAN 或雲端存取；
- 已知 mitigation；
- 遮蔽後的 log 或 capture。

禁止傳送真實 Wi-Fi 密碼、Tapo 密碼、MyDyson 密碼／OTP、TP09 MQTT credential、KLAP session material 或未遮蔽 `.secrets/`。

## 處理流程

Maintainer 會以 best-effort 方式：

1. 確認收到並建立私人聯絡管道；
2. 重現問題並評估嚴重性；
3. 準備修正與 regression coverage；
4. 協調揭露與 release 時間；
5. 在適當且回報者同意時提供 credit。

若需要硬體重現，請在公開揭露前保留合理調查時間。

## 安全模型與已接受邊界

以下是已公開的設計邊界，本身不一定構成漏洞：

- credential 儲存在一般、未加密的 ESP32 NVS；
- 能實體讀取 Flash 的人可能取得設定；
- 設定 AP 是暫時的本地 provisioning channel；
- 設備預期位於可信任且可互通的 IPv4 LAN；
- RG57A IR 狀態為推定，沒有 authentication 或 readback；
- MyDyson 雲端只用於首次取得 TP09 本地 credential；
- 專案不提供 internet-facing control、OTA 或遠端 Web API。

若實作缺陷超出上述邊界，仍歡迎回報，例如 secret 出現在 log、設定入口未依預期關閉、authentication bypass、不安全的 response parsing，或可從預期 LAN 外控制設備。

## Credential 已外洩時

1. 從公開位置移除，但應假設 Git history 與 cache 仍可能保留。
2. 若 Wi-Fi 或 Tapo 密碼外洩，立即更換。
3. 可行時重新 provision 或 rotate TP09 本地 credential。
4. 若 Flash 可能被複製，清除並重新設定 Cardputer。
5. 禁止把替換後的 credential 附在 issue 或 pull request。
