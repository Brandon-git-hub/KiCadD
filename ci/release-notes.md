<!-- Release notes template for tag builds (.github/workflows/windows.yml, job release).
     {{UPSTREAM}} is filled from patches/UPSTREAM_BASE; build info and SHA-256 are appended. -->
KiCadD 試用版：以 KiCad nightly（upstream {{UPSTREAM}}）為基礎的修剪版 Windows x64 build，加上 Agent（MCP）協同設計。

> ⚠️ Pre-release，僅供試用。由 GitHub Actions 自動建置；功能在 Linux 上做過端到端測試，Windows 上只有 CI 的 smoke test 與早期版本的人工試用。

## 這一版（alpha.4）
- **擺件時編輯器不再卡住：** `auto_place` 改在背景執行緒運算，編輯器可以照常平移、縮放、查看。運算期間 Agent 的其他修改會等擺件完成後才套用；你在運算期間自己改了板子，擺件結果會被捨棄（不會蓋掉你的修改），結果會註明。
- **在已佈線的板子上擺件：** 走線與過孔依實際形狀（含圓弧）和各自網路類別的間距當障礙，逐個焊盤檢查，不再用整個封裝外框，能放下的零件變多。真的放不下的零件留在原位，其他零件會避開它。實測已佈線的 `pic_programmer`：線長 1753 → 1653 mm，鋪銅後 DRC 從 563 個錯誤只剩 122 個未連線（走線需要重拉）。

## alpha.3 修正（alpha.2 試用回報）
- **`auto_place` 讓編輯器「沒有回應」：** 改善線長的階段現在有時間上限（`time_limit`，預設 10 秒，`-1` 不限），時間到就停，擺放結果仍然合法，結果會註明。零件已經在板上時，重複的起點會提早結束；收斂後也不再多跑。實測已佈線的 `pic_programmer`（`all`）：16 核 14.9 → 9.5 秒，4 核在 10 秒停止，線長相同。
- **DRC 的 `lib_footprint_issues` 警告雜訊：** 本版沒有 KiCad 官方庫，來自官方庫的封裝不再逐一警告。`pic_programmer` 從 63 個警告變成 0 個。專案自己的庫仍會比對，和庫不一致時照樣回報。
- **`sch_netlist`：** 多單元零件的共用腳只列一次；`#` 開頭的虛擬符號不列出；標了不接（X）的腳另外列在 `no-connect` 一行。

## 安裝 / 執行
1. 下載 `kicadd-windows-x64.zip`，解壓到任意資料夾（免安裝）。
2. 執行 `bin\kicad.exe`（專案管理器）。Agent 協同請從專案管理器開專案，再開電路圖編輯器（Ctrl+E）與 PCB 編輯器（Ctrl+P）：兩個編輯器在同一個行程、同一個 API 伺服器上。
3. 執行檔未簽章，Windows SmartScreen 會警告：「其他資訊 → 仍要執行」。

## 接上 Agent（MCP）
編輯器開著時，`bin\kicadd-mcp.exe` 會自己找到它的 API socket，不需要任何設定。

Claude Desktop（`%APPDATA%\Claude\claude_desktop_config.json`）或 Cursor（`mcp.json`）：
```json
{ "mcpServers": { "kicadd": { "command": "C:\\KiCadD\\bin\\kicadd-mcp.exe", "args": ["--stdio"] } } }
```
Claude Code：`claude mcp add kicadd -- C:\KiCadD\bin\kicadd-mcp.exe --stdio`（路徑換成你解壓的位置）。

## Agent 能做的事（26 個 MCP 工具）
- **電路圖**：`sch_add_part`（符號庫或立創編號）、`sch_connect` / `sch_disconnect`（網路名稱連接）、`sch_netlist`、`sch_delete`、`sch_save`；**階層圖紙** `sch_add_sheet`，`ports` 產生圖紙腳位，父圖紙上用 `圖紙名.腳位名` 連接。
- **立創零件**：`lcsc_fetch` / `sch_add_part lcsc=` 依編號抓封裝與符號，存進專案的 LCSC 庫；回報庫存、價格、基礎件 / 擴展件。編輯器裡也有 Place → Add LCSC Part / Symbol。
- **電路圖 → PCB**：`sync_pcb`（開著電路圖編輯器時直接讀編輯器內容，不會寫入你的電路圖檔）。
- **自動擺件** `auto_place`：依連線擺放、去耦電容靠 IC 腳；選用功能：模組分群（`groups` 或 Block 欄位）、去耦電容放背面（`back_decaps`）、擺件規則（`rules`：板邊、區域、角度、距離上限）。
- **PCB 編輯**：查詢、移動、翻面、新增 / 刪除封裝、DRC、鋪銅、存檔、Agent 自己的 undo / redo。
- **與你協同**：Agent 的修改就是編輯器的提交，你按 Ctrl+Z 就能撤銷；你在互動工具中（走線、畫線、對話框）時，Agent 的修改會被擋下，不會干擾你。走線由你來做。

## 這一版拿掉了什麼
| 拿掉 | 影響 |
|---|---|
| OpenCASCADE | 不能匯出 STEP / GLB / STL / U3D；3D 模型只讀 VRML（`.wrl`） |
| ngspice | 電路圖沒有模擬器（SPICE 模型欄位與 SPICE netlist 匯出仍保留） |
| Python | 沒有 Python 外掛，也沒有「External Plugins」選單 |
| 首次啟動精靈、更新檢查、外掛與內容管理器（PCM） | 第一次啟動直接進入；不連 KiCad 的伺服器 |

其餘的電路圖、PCB、推擠走線、差分對、等長、DRC、Gerber 匯出等都是 KiCad 原本的功能。

## 注意
- **沒有附 KiCad 官方元件庫。** 零件請用立創編號（Agent 或 Place → Add LCSC Part），或用你專案自己的庫。
- **想先隨便開個板子試：** `share\kicad\demos\` 裡有 KiCad 的範例專案（例如 `pic_programmer`），封裝已內嵌在板子裡。
- 立創資料來自 JLCEDA / EasyEDA 官方庫，使用條款以其網站為準；本版不附任何轉換好的零件。
- 設定檔位置與 KiCad nightly 相同（`%APPDATA%\kicad\10.99`），不影響已安裝的正式版 KiCad 9。
- 尚未支援：依規格搜尋立創零件、立創零件的 3D 模型、同一圖紙檔重複使用、連接器伸出板邊的擺放。
- 問題回報：Help → Report Bug（開 GitHub issue）。

## CI 驗證（smoke test）
`kicad-cli` 啟動、`pic_programmer` DRC、`rectifier` 匯出 SPICE netlist、API server 經 `kicadd-mcp --stdio` 回應 `board_summary`。
