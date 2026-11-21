# KiCadD 使用說明（Beta）

KiCadD 是以 KiCad nightly（10.99）為基礎的精簡版，加上讓 Agent 透過 MCP 一起設計電路圖和 PCB 的服務。
這份文件是 beta 的固定說明；每一版的改動寫在 release notes，開發紀錄在 [README](../README.md)，接下來要做的事在 [ROADMAP](../ROADMAP.md)。

## 1. 安裝

**Windows x64（release 的 zip）**
1. 下載 `kicadd-windows-x64.zip`，解壓到任意資料夾，不需要安裝。
2. 執行 `bin\kicad.exe`（專案管理器）。
3. 執行檔沒有簽章，Windows SmartScreen 會警告，請按「其他資訊 → 仍要執行」。
4. 設定檔放在 `%APPDATA%\kicad\10.99`，和 KiCad nightly 相同，不會動到已安裝的 KiCad 9 / 10 正式版。

**Linux：** 沒有預先編好的版本，請從原始碼編譯，步驟見 README 的「Build」。**macOS：** 不支援。

## 2. 接上 Agent（MCP）

有兩種用法：

| 用法 | 怎麼開 | 適合 |
|---|---|---|
| **桌面協同** | 從專案管理器開專案，再開電路圖編輯器（Ctrl+E）和 PCB 編輯器（Ctrl+P）。`kicadd-mcp` 會自己找到編輯器的 API socket。 | 你和 Agent 一起改同一份設計，你可以看著它改，按 Ctrl+Z 撤回 |
| **無介面（headless）** | `bin\kicad-cli.exe api-server --socket <名稱> <板子.kicad_pcb>`，再用 `kicadd-mcp --socket <名稱> --stdio`。 | 讓 Agent 自己跑：審查、出廠檔、批次修改、CI |

Claude Desktop（`%APPDATA%\Claude\claude_desktop_config.json`）或 Cursor（`mcp.json`）：
```json
{ "mcpServers": { "kicadd": { "command": "C:\\KiCadD\\bin\\kicadd-mcp.exe", "args": ["--stdio"] } } }
```
Claude Code：`claude mcp add kicadd -- C:\KiCadD\bin\kicadd-mcp.exe --stdio`（路徑換成你解壓的位置）。

`kicadd-mcp --http 127.0.0.1:8765` 會改用 HTTP 提供 MCP，同一個埠也有瀏覽器的 2D 檢視（`/`）。對外開放時請加 `--token`。

## 3. Skill：讓 Agent 知道怎麼用這些工具

58 個工具的搭配方式寫成一份 skill，內容包括先查再改、新板設定、審查、擺件、佈線、找料、出廠流程、undo、批次和常見陷阱。

- **Claude Code：** 把 `share\kicadd\skills\kicadd` 整個資料夾複製到 `%USERPROFILE%\.claude\skills\`（所有專案都能用）或專案的 `.claude\skills\`。
- **Claude Desktop：** 在設定的 Skills 頁面上傳 release 附的 `kicadd-skill.zip`。
- **其他 Agent（Cursor 等）：** 把 `SKILL.md` 的內容貼進它的規則檔或系統提示。

## 4. 存檔格式：10.99 的限制

- KiCadD 存的 `.kicad_pcb`、`.kicad_sch` 是 **KiCad nightly（10.99）格式**，KiCad 9 / 10 正式版可能打不開。
- **請在專案副本上試用。** 存檔前可以先用 `board_diff` 看這次改了什麼。
- Agent 的修改在 `save_board` / `sch_save` 之前只在記憶體裡。`sch_export_pdf`、`board_render`、`sync_pcb` 用的也是記憶體裡的內容，所以不需要先存檔。
- 一個服務同時只開一個專案（板子加電路圖），換專案請用 `open_board`。目前的板子有未存檔的修改時，`open_board` 會拒絕並列出 `board_diff` 的結果。如果只是 undo 回原狀，結果會寫「the same as the saved file」，這時加 `discard=true` 不會丟掉任何東西。桌面版沒有 `open_board`，請自己在 pcbnew 開檔。

## 5. 做得到 / 做不到

**做得到**

| 類別 | 工具 |
|---|---|
| 電路圖 | `sch_add_part`（立創編號、專案的符號庫（KiCadD 不附符號庫，圖上已用過的符號取其內嵌副本）、電源符號）、`sch_connect` / `sch_disconnect`、`sch_move`、`sch_wire`、`sch_delete`、`sch_set_fields`、階層圖紙 `sch_add_sheet`、`run_erc`、`sch_netlist` |
| 電路圖文件 | `sch_annotate`（重新編號，可以 dry run、限定圖紙或零件、設定起始號、每頁一段）、`sch_export_pdf`（所有圖紙合成一份 PDF，可點擊跳頁；在 Adobe Acrobat / Reader 點零件會跳出欄位選單，瀏覽器內建的 PDF 檢視器不支援）、`sch_render`（PNG） |
| 立創零件 | `lcsc_fetch`（封裝、符號、3D 模型（VRML）、庫存、價格）、`lcsc_search`（依規格找料）、`lcsc_match`（批次配料號）；編輯器裡的 Place → Add LCSC Part / Symbol |
| 自建元件 | `lib_create_symbol`、`lib_create_footprint`、`lib_register` |
| 局部模擬 | `simulate`（ngspice，附在 Windows 壓縮檔裡）：圈選零件、圖紙或某顆零件周圍，模型依零件索引的參數產生（R、C（MLCC 依介質、尺寸、容值與實際偏壓估 DC Bias，ESR / ESL 用典型值）、L、二極體、MOSFET、LDO、Op-Amp），或用零件自己的 Sim.* 模型；量測 op、tran（平均、漣波、階躍的過衝 / 上升 / 穩定時間）、ac（增益、峰值、−3 dB）、dc 掃描（I-V 曲線）；`plot=true` 附 PNG 圖。估算值一律標明，可逐顆覆寫或掛外部 .lib |
| 電源完整性 | `pdn`：電源軌在負載處的阻抗 Z(f)，電容的 ESR / ESL / DC Bias 加上板上實際的安裝電感（走線、過孔、平面），找反諧振尖峰並標出兩側的電容；給目標阻抗才列出超標頻段；可試加電容、改安裝電感 |
| 訊號完整性 | `reflection`：依實際佈線（每段走線的 Z0 與延遲）模擬邊緣，列出每個接收端的過衝、ringback、flight time、是否單調，並建議串聯端接電阻（Z0 − 驅動阻抗，E24）及加上後的結果 |
| 專案 | `new_project`：在伺服器上建立新專案（`.kicad_pro`、空白板子與電路圖，可指定層數、疊層、板框），不覆寫既有檔案；`open_board` 開既有專案 |
| 電路圖到 PCB | `sync_pcb`（照 UUID 對應，重新編號的零件原地改名）、`auto_place`（依連線擺放、去耦電容靠 IC，可設分群和擺件規則；USB、RJ45、DC 插座等連接器可依板邊規則朝外伸出板框） |
| PCB 編輯 | 查詢、移動、翻面、新增 / 刪除封裝、`pcb_set_fields`、`delete_tracks`、`board_outline`、`board_setup`（疊層，或 JLCPCB 的阻抗疊層）、`set_rules`（網路類別、JLCPCB 製程預設、依電流算線寬、依阻抗算線寬 / 差分間距）、`impedance`（各銅層的控制阻抗線寬）、`zones` |
| 佈線（逐條） | `route`（一條線，可以指定經過點和換層位置）、`route_short`、`fanout`、`stitch_vias`、`teardrops` |
| 審查與出廠 | `audit`（去耦、電源、ESD、電流對線寬、電壓對間距、絲印、測試點、參考平面：跨分割、平面缺口、回流地孔；差分對 skew / 耦合間距 / 未耦合長度與等長群組（只檢查，不繞線）；電源軌 DC 壓降（鋪銅、走線、過孔的電阻網路，最差負載、損耗分佈、最忙過孔）；平行走線串擾初篩（同層間距小於 3H 的耦合長度）；沿線阻抗剖面（每段依線寬、層與上下實際的銅算 Z0 / 差分對 Zdiff，與目標阻抗比對）；不給門檻時只回總覽，門檻由 agent 依網路萬用字元指定 mm）、`run_drc`、`board_diff`、`silk_tidy`、`board_render`（`marks` 標示熱點）、`export_fab`（先跑 DRC，再輸出 Gerber、鑽孔、BOM、CPL） |
| 協同 | Agent 的每次修改都是編輯器的一筆提交，可以按 Ctrl+Z 撤回；Agent 也有自己的 `undo` / `redo` / `sch_undo` / `sch_redo`。你正在用互動工具（走線、畫線、對話框）時，Agent 的修改會被擋下。`batch` 一次跑多個工具。 |

**做不到（刻意不做或還沒做）**

| 項目 | 狀態 |
|---|---|
| 全板自動佈線、差分對 / 等長的自動佈線（meander） | **不做。** `route` 只代替重複的手動佈線。差分對和等長之後會做成**檢查**（ROADMAP 階段二）。你可以照常用 KiCad 自己的差分對和等長工具手動佈線。 |
| 編輯器裡的模擬視窗 | 沒有（精簡 build 不含）；Agent 用 `simulate` 跑 ngspice（`plot=true` 回傳圖）。`simulate` 不含 PCB 寄生；電源的安裝電感在 `pdn`，走線的傳輸線效應在 `reflection` |
| STEP / GLB / STL 匯出 | 沒有 OpenCASCADE，3D 模型只讀 VRML（`.wrl`） |
| ODB++ / IPC-2581 / DXF、組裝圖 | 未支援（組裝圖在階段五） |
| 同一個圖紙檔重複使用（多實例） | 未支援（階段五） |
| Python 外掛、外掛與內容管理器（PCM）、首次啟動精靈、更新檢查 | 已拿掉 |
| KiCad 官方元件庫 | 沒有附。請用立創編號、自建，或專案自己的庫。`share\kicad\demos\` 有範例專案可以試（封裝已內嵌在板子裡）。 |
| MCP resources / prompts | 只有 tools（階段五） |
| macOS 版 | 沒有 |

## 6. 問題回報

在編輯器裡選 Help → Report Bug（開 GitHub issue）。回報時請附上：版本（release 的 tag）、桌面版或 headless、Agent 呼叫的工具和參數，以及工具的回應原文。
