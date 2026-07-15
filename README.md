# Cardputer Adv 區域網路家電控制器

目標是讓 Cardputer Adv 在不依賴 Home Assistant、外部伺服器或日常網際網路的情況下，控制 Tapo L530E、Dyson TP09 與台灣三洋 RG57A 冷氣。

Python 實機閘門已通過：L530E 使用 KLAP v2，TP09 的實際 product type 為 `438K`，兩者都已完成寫入、讀回與還原。Cardputer Adv 韌體會自動探索 Dyson 與同網段全部 L530E，不需設定固定 IP；主介面使用繁中選單統一操作。硬體回歸項目見 [硬體驗收清單](docs/hardware-acceptance.md)。

## 環境建立

PowerShell：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\setup.ps1
```

也可手動執行：

```powershell
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe -m pytest
```

## 先做唯讀測試

Cardputer、執行 probe 的電腦、L530E 與 TP09 應在同一個允許 UDP broadcast/mDNS 的 IPv4 LAN。帳密由隱藏輸入接收，不會寫入報告。

請從一般 Windows Terminal / PowerShell 應用程式執行；受限的 Codex 命令環境可能會擋住 LAN UDP 與 Dyson HTTPS。Probe 會自動排除 Tailscale `/32` tunnel，改用實體 LAN 的 directed broadcast。

```powershell
.\.venv\Scripts\python.exe .\probe.py all
```

如果 discovery 被路由器擋住，可指定 IP：

```powershell
.\.venv\Scripts\python.exe .\probe.py all --tapo-host 192.168.1.20 --dyson-host 192.168.1.21
```

如果同時找到多顆 L530E，probe 會停止而不隨機改變任一顆燈，並在報告的 `context.candidate_hosts` 列出 IP。從 Tapo App 的裝置資訊確認目標 IP 後，使用 `--tapo-host` 重跑。

失敗報告會以 `stage/code` 分類，例如 `dyson_provisioning/provision_failed`、`dyson_complete_login/otp_or_login_failed`、`tapo_authentication/l530e_seen_but_auth_failed`，不會寫入 request body 或帳密。

## 可逆寫入測試（韌體門檻）

測試會短暫改變燈泡與風扇狀態，並在正常與例外路徑中還原 snapshot。測試時請不要同時用 App 操作設備。

```powershell
.\.venv\Scripts\python.exe .\probe.py all --write-test --save-dyson-credential
```

- `probe-report.json` 是可分享的遮蔽報告，並被 Git 排除。
- `.secrets/dyson-local.json` 含 Cardputer 後續要匯入的本地 MQTT 資料，不可分享或提交。
- 若自動還原失敗，probe 會在報告中標示 `restore_ok: false`；請先用官方 App 確認狀態。
- Tapo Smart/KLAP 的 transition 參數在 `python-kasa 0.10.2` 中不支援，報告會忠實標為 `false`，不會把忽略參數誤報為成功。

MyDyson 雲端只用在這次 OTP 取得並解密本地 credential。日常韌體不會含 MyDyson 帳密或 cloud fallback。

已產生 `.secrets/dyson-local.json` 後，可不用 MyDyson/OTP 直接重做本地驗證：

```powershell
.\.venv\Scripts\python.exe .\probe.py dyson-local --write-test
```

RG57A 的預備 Midea 48-bit 映射見 [docs/rg57a-mapping.md](docs/rg57a-mapping.md)。第一次實際 IR 發射會在韌體階段進行。

## Cardputer 韌體

建置、首次設定、快捷鍵與燒錄檔說明見 [firmware/README.md](firmware/README.md)。一鍵重現建置：

```powershell
.\.venv\Scripts\python.exe -m pip install -r .\firmware\requirements.txt
.\scripts\build-firmware.ps1 -RunNativeTests
```
