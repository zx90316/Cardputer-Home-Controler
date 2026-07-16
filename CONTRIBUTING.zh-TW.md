# 參與 Cardputer Home Controller

感謝你協助改善 Cardputer Adv 上可靠、私密且本地優先的家電控制體驗。

[English](CONTRIBUTING.md) · 繁體中文

## 開始之前

本專案刻意優先考量：

- 日常運作維持 local-only；
- 使用 LAN 自動探索，不依賴固定 IP；
- 以設備真實確認為完成依據，不讓 UI 樂觀回報成功；
- IR 控制不受網路流量阻塞；
- 診斷資訊遮蔽與嚴格的敏感資料管理；
- 可重現的 PlatformIO 與 Python 環境。

必須架設伺服器、依賴 Home Assistant、日常雲端控制、輸出明文 credential 或寫死設備位址的變更，不符合目前專案方向。

Bug 請使用 [bug report 表單](https://github.com/zx90316/Cardputer-Home-Controler/issues/new?template=bug_report.yml)。大型設計或新增設備家族請先開 issue 討論，再投入完整實作。

## 開發環境

### Python probe

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\scripts\setup.ps1
```

或手動建立：

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe -m pytest -q
```

### 韌體

```powershell
.\.venv\Scripts\python.exe -m pip install -r .\firmware\requirements.txt
.\scripts\build-firmware.ps1 -RunNativeTests
```

韌體依賴固定於 `firmware/platformio.ini`。若要放寬版本，必須說明相容性理由並通過乾淨建置。

## Branch 與 commit

- 從目前 default branch 建立範圍明確的 branch。
- Bug fix PR 不要混入無關重構。
- Commit subject 使用簡潔的祈使句，例如 `Isolate IR dispatch from network polling`。
- 不要在沒有說明的情況下重寫或刪除其他貢獻者的工作。
- 禁止提交 firmware build、`.secrets/`、`probe-report.json`、credential、設備 serial 或私人 LAN 資訊。

## 程式規範

### C++ 韌體

- UI 不應被網路操作同步阻塞。
- 跨 task 狀態只能透過 mutex 保護的完整快照存取。
- 所有 `millis()` deadline 都必須使用支援溢位的時間 helper。
- 保留依屬性合併命令與 IR one-shot 正確順序。
- retry、payload、response buffer 與 blocking operation 必須有明確上限。
- 區分命令進入 queue、IR 已發射、MQTT 已確認與 KLAP 已讀回。
- Serial log 不可包含 Wi-Fi、Tapo、Dyson、cookie、token 或 credential。
- 必須通過 `firmware/platformio.ini` 已啟用的 compiler warnings。

### Python probe

- 所有 live write test 都要在正常與例外路徑保存並嘗試還原狀態。
- 使用穩定的 stage/code 診斷分類。
- Credential 只能透過 hidden input 或 `.secrets/`；報告必須保持 redacted。
- 底層 library 若忽略參數，不可將該功能誤報為支援。
- 要求真機之前，先補上 simulated-device coverage。

## 協定與設備變更

協定工作必須記錄：

1. 設備型號、hardware revision、firmware version 與 product type；
2. transport 與 discovery 行為；
3. 公開協定參考或獨立觀察到的行為；
4. authentication、integrity、replay、length 與 timeout 處理；
5. 確認語意與離線復原；
6. 第三方來源與授權影響。

不可複製沒有授權或授權不相容的 repository 程式碼。加入 library 或協定參考時必須更新 `THIRD_PARTY.md`。

## Pull request 必要測試

同時執行兩套測試：

```powershell
.\.venv\Scripts\python.exe -m pytest -q
.\scripts\build-firmware.ps1 -RunNativeTests
```

依變更類型提供相稱證據：

| 變更 | 預期證據 |
| --- | --- |
| Parser、crypto、payload 或 mapping | 固定 native vectors 與竄改／錯誤案例 |
| Queue、retry、timeout 或 task | Native state／pressure／wrap 測試 |
| Python probe | 單元或模擬設備測試，包含 redaction |
| UI | 按鍵流程說明；可行時附上目標螢幕照片 |
| 真實設備支援 | 遮蔽後的 probe report 與硬體驗收結果 |
| IR timing | 網路負載下的 Serial `latency_ms` 樣本 |

測試 fixture 禁止包含 credential 或未遮蔽 packet capture。

## 硬體驗證

請依 [docs/hardware-acceptance.md](docs/hardware-acceptance.md) 進行目標設備驗收。桌面測試無法證明 IR 距離、螢幕排版、IMU 喚醒、DHCP 復原或長時間 heap 穩定性。

若沒有真機，請在 PR 清楚標示，並將相關驗收項目保留為 pending。

## Pull request 內容

良好的 PR 應包含：

- 問題與 root cause；
- 聚焦的解法說明；
- 使用者可見的修改前／後行為；
- 驗證命令與結果；
- 已測試的 hardware 與 firmware 版本；
- security、migration 與 rollback 影響；
- 文件與硬體驗收清單更新。

Maintainer 可能要求縮小範圍、增加證據或補上協定參考後才合併。

## 安全問題

若問題可能暴露 credential、設備存取或 exploit 細節，禁止建立公開 issue，請依 [SECURITY.md](SECURITY.md) 回報。

## 行為準則

參與本專案即表示同意遵守 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。
