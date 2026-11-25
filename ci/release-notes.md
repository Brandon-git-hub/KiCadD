<!-- Release notes template for tag builds (.github/workflows/windows.yml, job release).
     {{UPSTREAM}} is filled from patches/UPSTREAM_BASE; build info and SHA-256 are appended. -->
KiCadD 試用版：以 KiCad nightly（upstream {{UPSTREAM}}）為基礎的修剪版 Windows x64 build，加上 Agent（MCP）協同設計。

> ⚠️ Beta（pre-release）。已能走完整個設計流程，功能仍會持續加入。由 GitHub Actions 自動建置；42 個端到端測試在 Linux 上全部通過（含桌面版），Windows CI 跑 smoke test 與 headless 的出廠、貼片、電路圖文件、模擬、PDN / 反射、新專案、開關 KiCad、工具、設定、undo 歷史測試；桌面版在 Windows 上只有人工試用。

## 使用前須知
完整的使用說明（安裝、接上 Agent、skill、存檔格式、做得到 / 做不到）：[docs/GUIDE.md](https://github.com/Brandon-git-hub/KiCadD/blob/main/docs/GUIDE.md)。

- **存檔格式：** KiCadD 以 KiCad nightly（10.99）為基礎。用 KiCadD 存過的 `.kicad_pcb`、`.kicad_sch` 會寫成新格式，**KiCad 9 / 10 正式版可能打不開**。請用專案副本試用；要知道這次改了什麼，存檔前先用 `board_diff`。
- **平台：** 只提供 Windows x64。Linux 可以自行從原始碼編譯，沒有 macOS 版。
- **不附 KiCad 官方元件庫：** 符號和封裝請用 `lcsc_fetch`、`lcsc_match` 從 LCSC 取得，或用 `lib_create_symbol`、`lib_create_footprint` 自建，也可以 `lib_register` 專案裡現有的庫。舊專案裡已經用過的符號（例如 `Device:R`），庫沒掛也能從電路圖內嵌的副本再放。
- **一次一個專案：** 一個服務同時只開一個專案（板子加電路圖），換專案請用 `project {open}`，新專案用 `project {new}`。
- **目前不支援：** 差分對與等長佈線、全板自動佈線（`route` 只代替重複的手動佈線）、電路圖自動排版、多實例重複圖紙、STEP 匯出與 STEP 模型（3D 只有 VRML）、ODB++ / IPC-2581 / DXF。

## 這一版（beta.13）
三部分：修正盲測 S2（四層 RP2040）回報的問題；收斂工具，從 68 個減為 58 個；跑一輪審查型盲測 R1（Jetson AGX Thor，10 層、1125 個零件），並修正它發現的問題。清單在 `docs/NEXT_beta13.md`。

- **工具合併（舊名稱已移除，沒有別名）：**
  - `undo {target, redo}` 取代 undo / redo / sch_undo / sch_redo。
  - `set_rules {}` 取代 get_rules。
  - `run_drc {refs, local: true}` 取代 check_drc。
  - `board_summary` 的最後一行就是完整的未連線數，取代 flush_ratsnest。這一行不等待忙碌的編輯器。
  - `project {open}` / `project {new}` 取代 open_board / new_project。
  - `export {fab, assembly, schematic_pdf}` 一次輸出多份，取代 export_fab / export_assembly / sch_export_pdf。
  - `board_render {view: "3d"}` 取代 board_render_3d。
- **審查型盲測 R1 之後：**
  - `board_summary` 列出 via 的種類（pad / drill），以及 through via 的最大縱橫比（板厚 / 鑽孔）。
  - `export {fab}` 列出 exclude-from-BOM 的零件，並標出接線像已裝、卻不是 DNP 的零件（沒人採購，也沒人貼）。
  - logo（board-only、沒有焊盤）的 `REF**` 不再被當成重複位號。
  - `audit sections` 的標頭只計算顯示出來的區段，`diff_pair` 會一併顯示 length 的列。
  - `board_render` 寫到不存在的資料夾時，會明確說明。
- **S2 的修正：**
  - `audit impedance` 標出沒耦合（uncoupled）的線段。
  - `route to=zone` 在平面上只打一顆過孔。
  - QFN 焊盤環內的過孔位置。
  - `auto_place` 的細腳距逃線環（`escape_room`）。
  - 從細腳距焊盤起線時自動縮窄線寬。
  - EasyEDA 電源腳型別的修正，以及 `lcsc_fetch pin_types`。
  - `sch_connect partial=true`。
  - 沿板邊的排針方向。
  - `board_render` 畫出 NPTH 孔。
- **圖示：** `kicadd-mcp.exe` 有了自己的圖示：兩層走線經一顆過孔相接。
- **測試：** 新增 `escape_e2e` 和 `merge_e2e`，共 42 個。

## beta.12
通用改進：beta.10 之後清單（`docs/NEXT_beta10.md`）的第一優先 10 項，對任何板子都有用的部分。工具數不變（68 個）。

- **`query_area` 的焊盤尺寸與過孔空位：**
  - 焊盤列加上銅箔的實際尺寸（旋轉後），例如 `PAD U1.5 [VDD] F.Cu @x,y 0.3x1.5`。
  - 新參數 `via_net`：在半徑內列出這個網路可放 through via 的空位，近的先列，彼此至少隔一顆過孔。空位條件：每一層都不撞其他網路的銅、不在焊盤上、不在有散熱焊盤的 IC 本體下、在板內。過孔尺寸用該網路的 class。
- **各工具的未連線數一致：** 所有工具（move、route、route_short、fanout、stitch_vias、undo、flush_ratsnest）用同一個函式計算，也用同一種寫法：延後的大網路寫 `excl. deferred nets: GND`，已計入的大網路寫出佔多少，例如 `unconnected=111 (incl. GND 24)`。以前 87 → 111 這種跳動看起來像弄壞了東西。
- **`set_rules` 的樣式比對：**
  - 每個 assign 樣式回報配到幾個網路。
  - 只因階層路徑（`/USBUP_P`）沒配到的網路會列出，並建議 `*USB*_P` 這類寫法。沒有網路配到時也會說明。
  - `classes` / `assign` / `constraints` 給成陣列時直接拒絕。以前會被忽略，回「nothing changed」。
- **電源腳的型別：** `lcsc_fetch` 把 `VD33`、`VD18`、`VD_PLL`、`VDD_1V8`、`VCC3V3`、`VDD33_USB` 這類腳判為 power_in（FE2.1 從 2 支變成 13 支），`VD33_O` / `VDD18_O` 仍是 power_out。可能是 IC 自己內部穩壓的腳（1V8 / PLL / CORE，同名只有一支），會提示 ERC 需要時加 PWR_FLAG。
- **LCSC 資料來源：** `lcsc_fetch` / `sch_add_part` 的庫存標明來源與日期（`EasyEDA, as of ...`），並附上零件索引（`lcsc_search` 讀的 jlcparts 資料）對同一料號的說法。兩者不一致時直接說出來，並說明哪一份比較新。
- **`sch_connect`：**
  - 選 stub 位置時，避開同一次呼叫裡剛放的 stub，也避開既有導線的中段。
  - 錯誤先報根本原因：哪支腳、哪個網路、被什麼擋住、在哪裡（例如 `the stub of R2.2 (P1) made in this call, at (209.55, 203.2)`）。網路合併改成一對網路一行，不再每支腳一行「asked +5V, on +1V8」。
- **NC 腳：** `sch_part` 的 no-connect 腳顯示 `NC`，不再顯示「(unconnected)」。`sch_netlist` 也把 no_connect 型別的腳列進 no-connect。
- **`lcsc_search` 的值：** 接受頻率（`12MHz`、`32.768kHz`）和電流（保險絲 `500mA`、電感 `2A`），以型錄寫法比對描述。電流也會比對額定電流欄。封裝可以只給名稱開頭的幾個詞：`LQFP-48` 找得到 `LQFP-48(7x7)`，`SSOP-28` 找得到 `SSOP-28-150mil`。
- **`auto_place` 與既有走線：**
  - 新參數 `track_room`。焊盤和其他網路的既有走線、過孔之間，除了間距，還要多留一條走線的空間（預設：class 的線寬加間距）。同網路的走線只需保持間距。零件不再被放進塞得下、卻接不出線的通道（第五輪：1.1 mm 的通道）。設 0 則照舊。代價：在已佈好線的板子上重擺全部零件時，線長會變長（pic_programmer 1863 → 2655 mm），但不會再有接不出線的焊盤（`track_room=0` 時有 65 個）。
  - 新參數 `keepout`：`[[x0, y0, x1, y1], ...]`，或 `{"area": [...], "side": "top"}`。框內不放零件，例如保留給佈線的通道。
  - 板上有走線時，回應會列出擺完後最擠的焊盤，以及有幾個焊盤旁邊連一條線都放不下。
- **pair order 看實際佈線：** 已佈好的差分對，若 P 和 N 的走線從上方看（不分層）沒有交叉，就另列為「繞過去」，例如 U 形逃線。這種情況不再一直被當成反序。真的交叉的會附上交叉點（`routed: crosses at x,y`）。
- **盲測 S1（一般密度兩層感測板）發現並修正：**
  - **`set_rules` 立即生效：** 新設的 net class 線寬與間距會馬上用在 `route` / `route_short`，不必重開板子。以前要重開才會生效，電源線因此被佈成板子最小線寬 0.127 mm。
  - **`route` 標出線寬來源：** 例如 `width 0.5 (netclass 'Power')`、`(netclass 'Default' diff pair)`、`(board minimum track width)`。
  - **`set_rules current`：** 小電流算出的線寬不會再低於板子最小值或 Default 的線寬，提高時會說明原因。class 參數打錯時，錯誤訊息列出所有可用的鍵。
  - **LCSC 腳型別：** 同名的穩壓輸出腳（AMS1117 的 VOUT 與散熱片）只把一支判為 power_out，其餘為 passive。以前一定會有一個 ERC 錯誤。`VDD/VDDA` 這種斜線分隔的腳名，任一個名字符合電源就判為電源腳。
  - **`fanout`：** 先重鋪銅再判斷。鋪銅孤島上的焊盤（仍有 ratsnest）不算「已接上」。
  - **`route to=zone`：** 焊盤已在鋪銅裡時，給了換層的 waypoint 就經過孔接到另一層的鋪銅（用於 starved thermal），不再拒絕。終點的鋪銅取最後一次換層的那一層。
- **測試：** route_e2e 新增 via 空位與 `auto_place` 通道兩組測試；set_rules、FE2.1 腳型別、NC、晶振 / 保險絲 / 封裝搜尋、`sch_connect` 同一次呼叫的 stub 碰撞，都加進既有測試。

## beta.11
新功能：階段四全部完成，包括第 1 項（桌面 Co-pilot 面板）、第 2 項（Web 視圖）、第 3 項的剩餘部分（由 MCP 開關 KiCad）。另有階段五的 3D 圖、組裝圖、MCP Resources / Prompts。工具 64 → 68 個。

- **`board_render_3d`（3D 圖）：**
  - 用 KiCad 自己的光線追蹤器（和 `kicad-cli pcb render` 相同）渲染記憶體中的板子，含未存檔修改。不需要 OpenGL 或螢幕。
  - 參數：`side`（top / bottom / front / back / left / right）、`rot_x` / `rot_y` / `rot_z` 傾斜、`zoom`、尺寸、`quality`（basic / high）、`floor`、`perspective`；`path` 可另存 PNG。
  - 這個 build 只載入 VRML 模型（LCSC 零件轉好的 `.wrl`），沒有本體的零件會依原因列出：沒有模型、找不到檔案、STEP。
- **`export_assembly`（組裝圖 PDF）：**
  - 正面一頁；背面有零件時再加一頁，從背面看（鏡像）。每頁都有圖框和標題欄，自動縮放到頁面。
  - 內容是 KiCad 的 Fab 層（焊盤外框、位號；`values=true` 加數值）和板框。
  - Fab 層沒有圖的零件（LCSC / EasyEDA 封裝），從 courtyard 補上外框、pin 1、位號。DNP 零件畫叉；logo 等不需組裝的封裝不加東西。
- **MCP Resources / Prompts：**
  - Resources：`kicadd://skill`（skill 內建在 kicadd-mcp 裡），以及它的 21 個章節 `kicadd://skill/{section}`，可以只讀需要的那節。另有 `kicadd://tools`、`kicadd://tool/{name}`、`kicadd://board`、`kicadd://schematic`。
  - Prompts：`review_design`、`new_board {spec}`、`prepare_fab`。
  - 沒安裝 skill 的客戶端也能取得完整的操作說明。

- **Web 視圖改為主動推送：**
  - KiCad 在 API socket 旁開 `<socket>.ev`（NNG PUB），有變更時通知。kicadd-mcp 只在收到通知時才去拿變更，取代原本每 50 ms 一次的輪詢。
  - 保底輪詢降為 1 秒；連不上通知時（舊版或 tcp socket）維持 50 ms。
  - 另一個 Agent 的編輯，幾毫秒內就會出現在瀏覽器。
- **Web 視圖的協同狀態：**
  - 新的「Agent」區塊：最近的編輯、被拒的請求、計數。
  - 可以從瀏覽器暫停或恢復 Agent 的編輯，和 Co-pilot 面板是同一個開關。暫停時標頭會顯示。
  - 「live」旁邊顯示同時有幾個人在看。
  - Web 視圖用自己的連線（`kicadd-web`），它的讀取不算進 Agent 的紀錄。
- **網路高亮：**
  - 在銅箔上雙擊，或游標指著按 H，高亮整條網路；其他部分變暗，並顯示焊盤數與走線長度。
  - 選取面板的網路名稱可以點。Esc 清除。
- **Schematic 分頁：**
  - KiCad 的 SVG 繪圖，可縮放、平移、換頁，點圖紙框可進入子圖。
  - 點零件會在板子上選取它，點導線會高亮該網路，兩邊同步。
  - Agent 改電路圖時自動重新載入。
  - 新請求 `SchSheetSvg`。
- **Chart 分頁：**
  - `simulate`、`pdn`、`reflection` 的圖表一律送到 Web 視圖（不必 `plot=true`；和 PNG 是同一份資料），保留最近 8 張。
  - 多面板、log 軸、目標線、游標讀值、圖例開關、滾輪縮放、雙擊回到全範圍。
- **`pdn` 認得 `C_100n_0402`、`C_22u_25V_0805` 這類值：** 以前 Jetson 底板的每個 rail 都找不到電容。
- **測試：** 新增 `client/webui_e2e.py`，用 headless Chromium（Playwright）實際操作頁面。

- **Co-pilot 面板：** PCB 與電路圖編輯器右側新增停靠面板（View → Panels → Co-pilot）。
  - **內容：** Agent 的編輯紀錄（含 undo / redo）、被拒絕的請求（同一個拒絕在幾秒內重複時合成一列並計次），以及狀態列（編輯數、被拒數、最後一次請求多久以前、是哪一種）。
  - **暫停：** 「Pause agent edits」勾選框，或 Ctrl+Alt+P（View → Pause Agent Edits）。暫停期間 Agent 的編輯立刻被拒絕（`paused: ...`，kicadd-mcp 不會像忙碌時那樣重試 10 秒），讀取照常。
  - **Agent 看得到暫停狀態：** `board_summary` / `sch_summary` 會加一行提示。
  - **共用：** 同一個 KiCad 程序裡的兩個編輯器共用紀錄與開關（狀態放在 kicommon）。
  - **焦點：** 點面板後鍵盤焦點會回到畫布，Ctrl+Z 等快捷鍵照常作用在編輯器。
  - **啟動時隱藏：** 設 `KICADD_COPILOT=0`。

- **`launch_kicad`：** 在 kicadd-mcp 已經連的 socket 上啟動 KiCad，之後所有工具直接作用在它上面。
  - **兩種模式：** `mode: "server"`（預設）是 headless 的 `kicad-cli api-server`；`"desktop"` 給人在旁邊看、檢查、一起改：有 `.kicad_pro` 時開 KiCad 專案視窗，並自動開啟 PCB 與電路圖編輯器，板子和 `sch_*` 工具都能用；只有 `.kicad_pcb`、沒有專案檔時單開 PCB 編輯器，只提供板子工具。
  - **路徑：** `path` 可以是專案資料夾（裡面唯一的 `.kicad_pro`）、`.kicad_pro`、`.kicad_pcb` 或 `.kicad_sch`。server 模式可以不給，先開空的，再 `new_project` / `open_board`。
  - **啟動後：** 等它回應（`wait_s`），回報開了哪塊板、哪份電路圖，以及 log 檔位置。
  - **限制：** 已經有 KiCad 在回應就拒絕。只啟動 kicadd-mcp 同目錄（或 build tree）裡的 KiCadD 執行檔，不接受任意命令。
- **`close_kicad`：** 只關 `launch_kicad` 啟動的程序，其他方式開的不動（拒絕）。板子或電路圖有未存檔修改就拒絕，並列出內容（附 board_diff）；`discard=true` 才照關。
- **HTTP 模式：** 兩個工具預設關閉，啟動 kicadd-mcp 時加 `--allow-launch` 才能用。stdio 模式一律可用。
- **桌面版的 socket：** PCB 與電路圖編輯器都讀環境變數 `KICADD_API_SOCKET`（先開的那個啟動 API 伺服器），所以能在指定的 socket 上服務；`KICADD_OPEN_EDITORS` 讓專案視窗載入專案後開啟編輯器。
- **`SchSummary.modified`：** 電路圖有未存檔修改時為 true。

## beta.10
修正版：第四輪盲測留下的 4–8 項、非直線的 via 搜尋，以及第五輪盲測發現的路由器當機、`route_short` 逾時與 `route` 的兩個問題。工具數不變（64 個）。

- **LED / 二極體的極性：** `sch_set_fields` 改 LCSC 料號或封裝時，比對符號的 A / K 腳與新封裝的焊盤編號。依據是該料號自己的 EasyEDA 符號，KiCad 的 `LED_*` / `Diode_*` 庫則依慣例（pad 1 = K）。對不上就警告 `polarity reversed`。例如 C2286 的符號 pin 1 是 A，C779449 的 pad 1 是 K。
- **`route` 貼著板邊：** 板邊間距改從 Edge.Cuts 線的中心量，和 KiCad 的 DRC 與路由器一致。以前多算了線寬的一半，正好貼著間距走的線會被當成違規而丟掉（「need 0.3 have 0.28」）。
- **`route_short` 非直線的 via 搜尋：** 沿兩端直線的位置都放不下 via 時，改在兩端周圍的格點找空位（零件旁的空隙），依繞行距離由短到長試。via 搜尋每次呼叫最多 45 秒，超過後其餘連線註明「vias not tried」，避免逾時（Run 5 第一次跑時一次呼叫超過 120 秒，之後的請求全部逾時）。
- **路由器當機：** via 或 waypoint 落在路線的起點、終點或前一個點上時，KiCad 路由器會在 `VIA::PushoutForce` 當掉（Run 5 第一次跑時發生，伺服器和未存的板子一起不見）。現在這種請求直接拒絕，格點搜尋也不再挑端點焊盤上的位置。
- **`route` 的微小線段：** 路由器在 waypoint 或焊盤旁留下的 0.1–5 µm 線段（四捨五入的殘餘）現在會刪除，並把相鄰線段接到保留的點上。
- **`route` 在 waypoint 之間繞行：** 兩個 waypoint 之間繞過障礙、超出兩點圍成的方框時，回應會說繞出多遠、在哪裡。以前沒有提示，下一條線常撞上它。`strict=true` 時直接拒絕這種路線。
- **`undo` 的未連線數：** undo / redo `set_rules` 或 `board_setup` 後，回報板子實際的未連線數（以前是 0）。
- **`auto_place` 的面：** 有給 `back` 或 `back_decaps` 時就由它決定面。已在底面、但兩者都沒指到的 SMD 零件會回到頂面，不再留在原來那面。
- **`lcsc_search` 關鍵字：** `16PIN`、`16-PIN`、`16POS` 也找得到型錄寫的 `16P`。`TYPE-C` 也比對 `TYPEC` 和 `Type C`。

## beta.9
新功能：階段三（區域模擬）的剩餘項目、由 MCP 新建專案，以及精簡工具說明。工具 60 → 64 個。另外依四輪子代理盲測（從規格從頭設計 7 埠 USB hub，與真人設計 StickHub 比對）修正與補強大量工具。

**工具說明瘦身（每輪少約一半 token）**
- **精簡清單：** 工具清單（`tools/list`）裡每個工具只留一兩句說明，參數說明各一行，整份從約 6.8 萬字元降到約 3.3 萬，減少 51%。
- **完整說明：** 搬到新工具 `tool_help {name}`，以及 skill 裡的 `TOOLS.md`（由 `kicadd-mcp --tool-docs` 產生）。
- **錯誤提示：** 工具第一次出錯時，回應會提示可用 `tool_help` 查完整說明。

**新工具**
- **`pdn`：電源軌的阻抗 Z(f)。** 在負載處計算電源軌阻抗。
  - **模型：** 每顆去耦電容都用 ESR + ESL + C_eff。C_eff 依軌電壓估 MLCC DC Bias。安裝電感取自板上的實際佈局：焊盤到過孔的走線、到平面的過孔迴路、平面對裡到負載的距離。另外加上平面對的電容、穩壓器（`vrm`）和負載自己的過孔。
  - **輸出：** 列出反諧振尖峰（附兩側的電容），以及各種電容的 SRF 與安裝電感的組成。給 `target` 才列出超標的頻段。
  - **試改：** `add` 試加電容，`models` 改安裝電感或排除電容。
  - **限制：** 平面當成集總元件，沒有腔體共振。
- **`reflection`：反射與串聯端接。** 依實際佈線模擬一個網路的邊緣：每段走線是一條傳輸線，帶該層、該線寬的 Z0 與延遲。
  - **量測：** 每個接收端的過衝、ringback、flight time、穩定時間，以及是否單調。
  - **建議：** 一個串聯電阻（Z0 − 驅動阻抗，取 E24），附加上後的結果。
  - **既有串聯電阻：** 網路上已有串聯電阻時，會到另一側找驅動端。
  - **驅動端參數：** rise、rout、vdd、cin 請照資料表給，沒給就用假設值並註明。
- **`new_project`：新專案。** 在伺服器上建立 `.kicad_pro`、空白的 `.kicad_pcb` 與 `.kicad_sch`，並接著開啟。
  - **可設定：** 層數、疊層（可用 JLCPCB 的阻抗疊層）、矩形板框，最後由 KiCad 存檔。
  - **安全：** 已有任一個檔就拒絕，不覆寫。目前的板子有未存檔修改時也拒絕，除非 `discard=true`。

**盲測回饋（子代理從規格從頭設計 7 埠 USB hub，與真人設計比對後的修正）**
- **`delete_tracks`：** 同時給 `nets` 與 `region` 時，以前會忽略區域、刪掉全板該網路。現在兩者交集，回應會逐網路列出刪了什麼。
- **`board_render layers:["back"]`：** 現在就是從底面看（底面絲印、鏡像），與 `back=true` 相同；front 與 back 同時給會拒絕。
- **`query_area ref=`：** 第一行就是零件本身（位置、面、角度），再列周圍。
- **DRC 的層：** 底面零件的焊盤以前標成 F.Cu，現在標實際的層（未連線標兩端各自的層）。
- **`move_footprints` 絕對擺放：** `parts {"U1": {x, y, rotation, side}}`，多顆一次、一步 undo。
- **`auto_place`：**
  - `spacing` / `edge` 給 0 會照用（以前被當成預設值）。
  - 新增 `silk_room=false`：只保留 courtyard（與 DRC 相同），密集的雙面板放得下了。
  - 「no room」改為分面列出面積。
- **`route to:"zone"`：** 可接到該網路自己的鋪銅；同層直接接，否則在端點打過孔，不打在焊盤上，並依序試最近的點。
- **`route` 的回報：**
  - 長度比改為對端點直線；有經過點時另列對經過點路徑的比。
  - 不再留下零長度線段。
  - 「in the way」同時列出物件位置與碰到的位置。
- **LCSC 符號的腳位型別：** 依腳名保守推定：GND/VCC/VDD/VIN 為 power_in；穩壓器 / 開關的輸出與 IC 內部穩壓輸出為 power_out；連接器與其他腳為 passive。`lcsc_fetch` 會說明推定結果。
- **`lcsc_search`：** 預設 live 檢查從 3 筆降為 1 筆（與 `lcsc_fetch` 共用 EasyEDA 的頻率限制）。
- **skill：** 補上下列建議：
  - 雙面與密集板的擺件規劃。
  - 底面 IC 會使差分對順序反向。
  - 兩層板做 90 Ω 的取捨。
  - 大電路圖要分圖紙。
  - JST 類外接埠的 ESD 要自行確認。

**第二輪盲測後的修正與新增**
- **工具參數容錯：** 字串型的物件 / 布林 / 數字參數會依 schema 轉型（client 持有舊版工具清單時也能用新參數）。
- **`lcsc_match`：**
  - `pick` 會比對索引中的值與封裝，不符就不採用並說明。
  - 套用時一併改寫舊零件的 Supplier Part / Manufacturer Part，以及提到舊料號的 Description / Datasheet。
- **`sch_add_part`：** 給的值與 LCSC 料號的值不同時警告。
- **`lcsc_search`：** 多個關鍵字找不到時，指出哪個字沒有任何結果，並去掉它重找。
- **`impedance ground_gap`：** 單線共面波導接地（CPWG），用 KiCad 的 COPLANAR 計算；差分對旁鋪地不計算並說明。
- **`audit` 差分對的 pair order：** 指出 P / N 在兩顆零件間交換的差分對，例如底面 IC 鏡像造成的交叉，未佈線的也檢查。
- **`audit sections`：** 只輸出指定段落，另有 `drc`（全板 DRC 分類計數）與 `unrouted`（各網路未佈線數）。
- **`route` 帶 `[x, y]` 目標：** 優先接在 `layer` 指定層上的銅（途中有換層時不限）。
- **`route_short` 自動打過孔（預設開，`vias=false` 關）：**
  - 兩端在不同層時，途中打一個過孔。
  - 同層但一層走不通時，繞到另一層再回來（兩個過孔）。
  - 沿直線依序嘗試多個位置。
- **`auto_place` 邊緣規則：** 接線用連接器（JST SH 等，訊號腳在後、固定腳在前）依焊盤判斷開口方向。
- **`sync_pcb`：** 換封裝時回報為「changed」並註明轉向 / 位移，不再算成新增。
- **`audit` 阻抗列：** 寫明量測長度，並說明 breakout 與離開平面的部分不計。
- **LCSC：**
  - Datasheet 取該料號自己的產品頁，不再沿用共用封裝的連結。
  - 連接器也依分類判斷（如 USB-C 的參考名 USBC），腳位全為 passive。
- **`board_render nets`：** 可用萬用字元（`USB_*`）。

**第三輪盲測後的修正**
- **`sch_move` 不再默默改接網路：** 原理圖以座標重合判定連線，零件移到密集區時，腳位、stub 端點或標籤可能剛好落在別的網路上而併網。現在移動前後逐腳比對網路（全圖所有零件），只要有腳改網就拒絕並整個回滾，回應列出每支腳的變化（例如 `U6.3: (none) -> /USB5_N`）。`allow_rewire=true` 照做，但一樣列出變化。
- **`sch_add_part` 給 x / y 時：** 腳落在既有的腳、導線或標籤上會警告，並列出設到的網路。
- **`sch_wire`：** 把兩個有名字的網路接在一起時警告。
- **`sch_undo` 一次移動：** 以前 undo `sch_move` 可能弄丟搬過的標籤（移除與加回的是同一個 id），已修正。
- **`batch` 保留警告：** 預設每步只回第一行，後面的警告（例如料號與值不符）會被吃掉；現在含 `warning:` 的行一併保留。第三輪的 4.7 µF 沿用 100 nF 料號沒有警告，就是這個原因。
- **`sch_set_fields` 改 `LCSC`：** 與 `lcsc_match` 一樣，Supplier Part / Manufacturer Part 及提到舊料號的 Description / Datasheet 一併改寫（移除 LCSC 時清除）；同一次呼叫中指定的欄位不動。
- **pair order 與 USB-C：** 同一網路在一顆零件上有多個焊盤時（USB-C 的 A6 / B6、A7 / B7），改取朝向對方零件最近的焊盤判斷，不再誤報上游差分對反序。
- **擺件後自動回報 pair order：** `move_footprints`、`auto_place` 移動的零件造成差分對 P / N 反序時，回應附一行說明；沒問題時不寫。skill 補上底面連接器的例子。
- **`route_short`：**
  - 過孔不再打在有散熱焊盤的 IC 的焊盤範圍內，例如 QFN 的 pad ring 與 EPAD 之間。
  - 不存在的網路名稱改為整個請求拒絕，並列出相近的名稱（例如 `VDD18` → `/Hub/VDD18`）。
- **`sch_connect` 真正全有或全無（第四輪）：** 同一次呼叫的 stub、標籤或電源符號落在別的腳或網路上（兩個要求的網路併成一個、沒要求的空腳被接上）時，整批拒絕並回滾，列出腳位（例如 `GND -> +1V8 for 83 pin(s) not asked`）。以前只警告、照接。
- **`route_short` 過孔禁區只限有散熱焊盤的 IC（第四輪）：** QFN 類（散熱焊盤至少是其他焊盤的兩倍大）的焊盤範圍內不打；LQFP 等沒有散熱焊盤的本體下照常可打（底面 QFP 逃線需要）。`allow_under_ic=true` 連 QFN 也試。
- **pair order 標明是擺件檢查：** 比的是焊盤間的直線，不看已佈的銅，佈線後仍會列出；佈好的差分對請看 skew / uncoupled 與 DRC。
- **工具回應的 UTF-8：** 欄位截斷不再切到多位元組字元中間（例如 `10KΩ`），以前會讓 kicadd-mcp 結束。

**`simulate` 新增**
- **`plot=true`：** 回傳 PNG 圖。
  - tran：波形，另一格畫電流。
  - ac：增益與相位（log f）。
  - dc：對掃描值作圖。
  - `corners`：上下限畫成淡線。
  - 圖由 KiCadD 自己畫（KiCad 的筆畫字型），不需要系統字型。
- **`dc` 掃描：** 掃描一個電源（V）或負載（A），電源電流即 I-V 曲線。
- **讀得懂寫成料號的值：** 例如 `CC0402_100NF_16V_10%_X7R`（`pdn` 也用同一套）。

## beta.8
修正版：把 60 個工具全部跑過一輪後回報的問題，以及 beta.7 試用時發現的問題。工具數不變（60 個）。

**修正**
- **`sch_delete` 留下孤兒電源符號：** 用 `sch_connect power=[...]` 接過電源的零件，刪掉後電源符號（`#PWR…`）會留在原地，ERC 報 `pin_not_connected`。現在電源符號會跟著導線一起刪掉。
- **`sch_connect` 全有或全無：** 只要有一支腳出錯，整批都不接（以前其他腳照接，修好後重試會遇到一半已接好的狀態）。已在別的網路上的腳，錯誤訊息會寫出那個網路的名字（以前常是 `already on net ?`：同一次呼叫裡，前面接上的腳讓連線資料過期了）。同一支腳在同一次呼叫裡被指定到兩個網路，也會說明。
- **`sch_connect` 避開文字重疊：** 標籤或電源符號會往外延伸，避開其他標籤、零件本體與位號 / 值的文字；真的沒有空位時照接並警告。延伸的導線也不會再穿過其他腳的端點（以前可能把那支腳一起接進網路）。
- **`move_footprints` 的「drc: clean」：** 零件移到有鋪銅的地方、敷銅還沒重填時，局部 DRC 比的是舊的敷銅形狀。現在會寫「drc: incomplete」，並提示先 `refill_zones` 再 `run_drc`。
- **`auto_place` 不再把已放好的連接器當成新零件：** 沒給 `refs` 時，只擺還沒放上板子的零件。以前凸出板邊的連接器（例如 USB-C）會被當成新零件，回報「no room for J1」。現在焊盤都在板內的零件就算已放好。
- **`lcsc_match` 認得 EasyEDA 的尺寸寫法：** `LED-SMD_L1.6-W0.8-R-RD` 這類名稱，從長寬（mm）對應到 0603 等尺寸。
- **`lcsc=` 不再改寫共用的封裝檔：** `lcsc.pretty/R0603.kicad_mod` 這類封裝檔由同封裝的所有零件共用，以前每取一次料就改寫值與說明，造成 git 雜訊。現在共用檔只寫封裝名稱；板上零件的值與 LCSC 料號不受影響。
- **`simulate` 的 LDO PSRR：** 以前 ac 在 10 Hz 會得到 −149 dB：資料上的 PSRR 又被迴路增益再壓了一次。現在低頻就是資料的數字，`psrr_f`（假設 loop_bw / 10）以上每十倍頻差 20 dB，最差到 `psrr_hf`（假設 20 dB），兩者都標成假設值，可用 `models` 覆寫。
- **`simulate ac` 的字元波形改用對數頻率：** 以前線性分格，最後一個十倍頻以前的點全擠在第一格。
- **`audit` 的 `currents given for 2 of 1`：** 分子分母現在數同一批軌道；給了電流但沒有電源零件、被排除的軌道會另外說明。
- **工具參數說明：** `route_short`、`fanout`、`teardrops`、`delete_tracks` 的 `nets`，以及 `zones` 的 `nets` / `layers` / `ids`，以前都寫成「Reference designators」。

**新功能**
- **`kicadd-mcp --version`：** 印出版本，例如 `kicadd-mcp 0.1.0-beta.8`。MCP 的 `serverInfo.version` 以前一直停在 `0.1.0-alpha.13`，現在由 tag 帶入。

## beta.7
修正版：本地試用 beta.5、beta.6 時回報的問題。工具數不變（60 個）。

**修正**
- **`simulate` 的 Op-Amp 被排除：** 從 LCSC 轉來的符號腳名是 `INVERTING INPUT`、`NONINVERTING INPUT`，以前認不得，所以用 `lcsc=` 加進來的這類運放都無法模擬。現在也認得這種寫法，以及 `IN+`、`INA+`、`IN A+`、`OUTA`；KiCad 原生庫沒有名字的輸出腳，改依腳位型別判斷。
- **多通道配對：** 依序用 unit（A / B）、腳名的通道字尾（`INA+` / `OUTA`、`1IN-` / `1OUT`）配對。同一個 unit 裡腳名重複時，每個輸出配符號上畫在它旁邊的輸入；拿不到位置時才退回用最近的腳號。這兩種是推測出來的，模型那一行會註明，例如 `inferred pairing: OUT 1 with +6 -7; OUT 8 with +2 -3 (by place in the symbol)`。
- **`simulate` 失敗時看不到原因：** probe 指到的網路上沒有任何有模型的零件時，以前只回 `no such vector n_BUF`；現在直接說明，並附上 left out 清單。其他 ngspice 執行失敗也會附上這份清單。
- **`simulate ac` 沒算 MLCC 的 DC Bias：** ac 前的 DC 預跑以前沒取到電容電壓，每顆 MLCC 都被當成 0 V、沒有衰減。現在和 `op`、`tran` 一樣，例如 `@ 3.3 V: C_eff ≈ 2.24 µF (est. ...)`。
- **`lcsc_match` 認得 LCSC 庫的封裝名稱：** `R0603`、`C0805`、`LED0603-RD` 這類名稱（`lcsc=` 產生的封裝）以前回「not a chip package」，現在讀得出尺寸。`0603Metric` 仍視為 0201。

## beta.6
新增 1 個工具（共 60 個）。壓縮檔裡多附了 ngspice（BSD 授權，`bin\ngspice\`）。

**新功能**
- **`simulate`：局部電路的 headless 模擬（ROADMAP 階段三第 2 項）。** 圈選範圍有三種：`refs`、`sheet`，或 `around` 某顆零件 / 網路往外 `hops` 步。電源網路只帶進兩腳零件，所以 `around ["U1"]` 會拿到 LDO 和它的輸入、輸出電容，不會把整片板子拉進來。
  - **模型依零件的 LCSC 參數產生：** R、C、L（含 DCR、SRF）、二極體與 Zener、MOSFET（level 1，含 Ciss / Coss / Crss 與本體二極體）、LDO（Vout 或 FB 參考、Dropout、Iq、PSRR）、Op-Amp（GBW、Slew、Vos、Ib、軌到軌）。零件已有 `Sim.*` 模型就直接用 KiCad 的。
  - **MLCC 的 DC Bias：** 依介質、封裝體積、容值和實際偏壓估算。先由「容值 ÷ 體積」推出介電層厚度，再用電場 V/d 算剩下多少容值，所以同樣是 0402，10 µF 比 0.1 µF 掉得多。報表一律寫成例如「`10 µF MLCC X5R 0402 6.3 V @ 3.3 V: C_eff ≈ 2.24 µF (est. DC bias: 22%, ...)`」。
  - **ESR / ESL：** 用封裝與容值的典型值，標「~」與「est.」。
  - **其他估計值：** LDO 的迴路頻寬、資料裡缺的 GBW 等也會標「assumed」。
  - **覆寫：** `models` 逐顆覆寫任何參數，或掛外部 `.lib` 的 subckt。
  - **電源、負載與分析：** 電源 `sources` 可給 DC、pulse、sin、ac；沒給的電源依網路名稱猜電壓（例如 +3V3），並註明。負載 `loads` 可給電流、脈衝或電阻。分析有 `op`、`tran`、`ac`，`corners` 用公差上下限再跑兩次。
  - **量測：** 平均、漣波 p-p、min / max 與發生時間、階躍的過衝 / 10–90% 上升 / 穩定時間、−3 dB 點、電源電流與功率、電感峰值 / 有效值電流（附 Isat / Irated）、LDO 輸出電流（附 Iout），另附一行字元波形。只量測、不下判定。
  - **沒有模型的零件：** 列為 left out，它的網路在那裡斷開，並說明。
- **`lcsc_search`：** 用 `where` 篩 Isat 又勾 `basic_only` 時，結果會是空的（資料裡沒有任何 basic / preferred 電感填了 Isat）。現在會說明原因，並建議改用 Irated。
- **`lcsc_search` 依電氣參數找料（ROADMAP 階段三第 1 項）：** 零件索引加入 MOSFET、LDO、Op-Amp、電感、二極體的二階參數（Rds(on)、Vgs(th)、Qg、Ciss/Coss/Crss、Dropout、Iq、PSRR、GBW、Slew、Vos、DCR、Isat、SRF、Vf、trr、Vz… 共 38 項，附資料表上的測試條件，例如 `Rds(on) 17 mΩ @10V`）。每筆結果列出這些參數；`where` 用門檻篩選，例如 `{"rds_on": "<=20m", "vds": ">=30", "vgs_th": "1..2.5"}`。缺該參數的零件不會列入：電感的 Isat 約一半有、SRF 約四分之一有，二極體的 Cj 與 MLCC 的 ESR / DC Bias 都沒有資料（填寫率見 [docs/PARAM_COVERAGE.md](https://github.com/Brandon-git-hub/KiCadD/blob/main/docs/PARAM_COVERAGE.md)）。需要新版零件索引：舊索引用 `where` 會提示 `update=true`。

**變更**
- **`audit plane` / `return_via` 跨層找參考平面：** 以前只看緊鄰銅層的鋪銅，現在和 `impedance`、`audit impedance` 用同一套參考平面（依鋪銅判定，跨過中間的訊號層；整板還沒有平面時仍取緊鄰銅層）。標頭會列出跨層的層，例如「Top Layer -> L4GND (across L2GND, L3)」。兩層訊號層相鄰的板子，數字會和以前不同。
- **新增提示：線下夾著局部銅皮。** 走線和主參考平面之間的訊號層，如果有局部鋪銅壓在線下（0.5 mm 以上），會報「over local copper」：總長、最長一段、是哪個網路的銅、在哪一層。高頻回流會走在那塊銅上，阻抗在那裡會跳一階（用 `audit impedance` 的 `z_nets` 看實際 Z）。這項只提示，不下判定；給了 `plane_gap` 時，被判定的網路另有一行說明。平面缺口處如果有局部銅，也會註明。

## beta.5
修正版：Windows 上從空白專案做 USB-C → AMS1117 → LED 小板，以及拿舊專案試用時發現的問題。工具數不變（59 個）。

**修正**
- **`power:PWR_FLAG` 會把電源網路短路（請更新）：** 以前產生的 PWR_FLAG 是一個腳名叫 PWR_FLAG 的電源符號，接上的 +5V、+3V3、GND 會併成同一個網路 PWR_FLAG，ERC 卻不報。現在和 KiCad 官方的定義相同（電源輸出腳、本身不成網路、位號 `#FLG01`…），可以用來消掉 `power_pin_not_driven`。`sch_connect` 的 `power` 清單寫 PWR_FLAG 會被拒絕並說明用法。**用 beta.4 以前版本放過 PWR_FLAG 的電路圖，請刪掉重放後再看 `sch_netlist`。**
- **新增檢查：標籤被電源符號蓋掉。** 標籤直接接到另一個名字的電源腳時，網路會被改成電源的名字、並和所有同名網路相連，KiCad 的 ERC 沒有這項檢查。現在 `sch_connect` 當場警告，`run_erc` 報 `kicadd_power_overrules_label`（錯誤）。以 7 個舊專案驗證沒有誤報。
- **`run_erc` 列出被忽略的檢查名稱：** 分成專案自己關掉的，和 KiCad 預設就關掉的（`single_global_label`、`four_way_junction`、`simulation_model_issue`、`footprint_filter`）；以前一律寫「ignored by the project's ERC settings: 4 check(s)」。
- **`auto_place` 不再搬動安裝孔：** `board_outline` 的 H1…、`MountingHole_*` 封裝與只有非電鍍孔的封裝，預設留在原位（`all` 也一樣），要搬請寫進 `refs`。以前角落孔的外框超出板邊一點，會被當成板外零件搬進板中。
- **`auto_place` 的 `rules` 寫錯會報錯：** 少了外層 `edge`（`{"J2": {"side": "left"}}`）、板邊規則裡打錯字，以前直接被略過；現在報錯並提示該放哪裡。
- **`sch_add_part` 找不到符號：** KiCadD 不附符號庫，舊專案的 `Device:R` 這類 lib_id 以前只回「not found」。現在電路圖已經用過的符號會從內嵌的副本放（共用同一份，不會多存）；真的找不到時，說明是庫表裡沒有這個庫、還是庫裡沒有這個符號，並列出這張圖同類零件用的符號（例如 `CM5IO:R (19)`）。工具說明不再以 `Device:R` 當範例。
- **`sync_pcb` 少報雜訊：** 新加入的封裝，原本元件庫的值與製造屬性改成電路圖的設定，不再列成「Changed R1 value from 1k to 5.1k」，也不會誤列進「被丟掉的製造屬性」。
- **`audit impedance` 的 breakout 從銅邊量：** 各網路自己的 pad 周圍 0.5 mm 不計時，改從 pad 的實際銅形往外量（以前從中心畫圓，長條 pad 窄邊外會多排除）。cm5 的 HDMI DDC 計入的長度因此多約 0.2–1.4 mm。

## beta.4
ROADMAP 階段二（版圖感知檢查）的第二到第五項，階段二至此完成（規劃見 [ROADMAP](https://github.com/Brandon-git-hub/KiCadD/blob/main/ROADMAP.md)）。四項都併入 `audit`，工具數不變（59 個）；和 beta.3 一樣，不給門檻時每項只回一行總覽、不下判定，門檻由 Agent 依網路萬用字元指定。

**新功能：`audit` 的四項檢查**
- **`diff_pair` / `length`：差分對與等長（只檢查，不繞線）。** 長度用 KiCad 自己的長度計算（和網路檢視器相同：走線、過孔跨距、pad-to-die）。差分對每 0.1 mm 量一次到對方的間距，依層取目標，報 skew、未耦合長度、間距範圍與過孔數。門檻：`skew`（指出短的那條要補多少）、`uncoupled`、`pair_gap`、`length_match`（每個 glob 一組，列出每條要補的長度）、`max_length` / `min_length`。
- **`ir_drop`：電源軌 DC 壓降。** 每個電源軌用鋪銅（取 fill，已扣掉 antipad）、走線、過孔與通孔建立電阻網路求解，報最差負載的壓降與位置、損耗分佈（走線 / 鋪銅 / 過孔）、最忙的過孔、負載最重的走線（給了電流時附 IPC-2221 所需線寬）。來源自動猜並註明，可用 `ir_source` / `ir_loads` 覆寫；格點大小會寫在每一行。門檻：`ir_drop`（mV）、`via_current`（A），要先用 `currents` 給電流。`board_render` 新增 `marks`，可在圖上圈出熱點。
- **`crosstalk`：平行走線串擾初篩。** 同一層、兩個不同訊號網路平行（10° 以內）且邊到邊間距小於 3H 的耦合長度，依網路對加總，分層列出，附最近間距（mm 與 H 的倍數）。差分對兩條合為一個名稱，電源與地不列。門檻：`xtalk_length`（mm）、`xtalk_spacing`（H 的倍數）。只看同層；相鄰兩層都是訊號層時，標頭會提醒寬邊耦合沒有篩。
- **`impedance`：沿線阻抗剖面。** 每條訊號線逐段計算（差分對每 0.25 mm），依實際線寬、所在層，以及該處上下實際的銅：中間訊號層的局部鋪銅壓在線下時以它當該處的平面，平面有缺口時取更遠一層。差分對依實際間距算 Zdiff，算法與 `impedance` 工具相同。總覽列出差分對與單線的主 Z 範圍、偏離最多的一條及原因（換層、線寬、局部鋪銅、平面缺口）。門檻：`z_target`（Z0）、`z_diff_target`（Zdiff）、`z_tolerance`、`z_off_mm`。過孔、pad 與同層共面地沒有計入。

**變更：參考平面依鋪銅判定（`impedance`、`set_rules`、`crosstalk`、`audit impedance` 共用）**
- 以前取緊鄰的銅層當參考平面，兩層訊號層相鄰時，H 會偏小（阻抗算低、串擾漏篩）。現在某層的鋪銅佔板框 40% 以上才算平面；還沒鋪銅的層，Board Setup 標成 power / mixed，或整層沒有走線（待鋪的平面）也算。不看層名，也不只看層類型。
- 往上、往下各找最近的平面，中間跨過的訊號層銅厚會算進 H，輸出標「across In2.Cu」；內層只有一側有平面時當 embedded microstrip。`impedance` 的 note 會列出每層的覆蓋率。
- **影響：** 依舊版算出線寬的板子，如果有兩層訊號層相鄰，請重跑 `impedance` 或 `set_rules impedance`。

## beta.3
ROADMAP 階段二（版圖感知檢查）第一項（規劃見 [ROADMAP](https://github.com/Brandon-git-hub/KiCadD/blob/main/ROADMAP.md)）。

**新功能：`audit` 的參考平面檢查（工具數不變，59 個）**
- **`plane`：跨分割、平面缺口。** 沿每條訊號線找上下相鄰銅層的鋪銅網路，量出走線從一個平面網路跨到另一個（分割）、經過灌銅的缺口，或超出平面邊緣的長度。
- **`return_via`：換層的回流過孔。** 訊號過孔換到參考平面不同的層時，量出連接兩個平面的同網路過孔（或通孔焊盤）最近多遠、過孔另一側（差分對以兩個過孔的中點為準）最近多遠、是否在 BGA 區內以及區內有幾個地孔；兩邊平面是不同網路（例如 +5V 對 GND）時，量兩網路之間最近的電容。
- **給 Agent 判斷：** 不帶門檻時只回總覽、不下判定；預設每段只有一行標頭，寫出最差的網路（約 200 token）。要逐行看用 `plane_limit`、`plane_nets`（網路萬用字元）、`pairs_only`（只看差分對，差分對合為一行）。門檻由 Agent 依網路萬用字元指定 mm：`return_via {"*PCIE*": 1.0, "*": 3}`、`plane_gap`、`two_sided`（要求兩側都有回流過孔）、`bga_return_via_mm`；完整名稱優先於萬用字元，越具體的萬用字元優先。判定依規則分組、最嚴的在前，通過的規則也有一行計數。skill 附上分級建議（嚴格 ≤1–1.5 mm、標準 ≤2 mm、寬鬆 ≤3 mm）。
- 以 cm5_minima、stickhub、pic_programmer、vme-wren、video 驗證，另跑過 jetson、coldfire 等板；vme-wren（12 層）全板 1.6 秒。

**修正**
- **`sch_add_part` 加到剛新增的圖紙：** 以前會誤報「the sheet's file is used more than once」，現在只計算實際顯示這個檔案的圖紙路徑。
- **`auto_place` 說明放不下的原因：** 以前只寫「no room for」，現在會比較零件面積和板框內可用面積（太小、太擠、或被規則 / 走線擋住），並列出任何旋轉都放不進板子的零件。
- **電路圖 PDF：** 說明點零件的欄位選單需要 Adobe Acrobat / Reader。

## beta.2
ROADMAP 階段一（交付閉環）的三項（規劃見 [ROADMAP](https://github.com/Brandon-git-hub/KiCadD/blob/main/ROADMAP.md)）。

**新功能（工具共 59 個）**
- **`impedance`：控制阻抗線寬。** 依板子的疊層算出每個銅層的單端線寬（`z=50`）、差分線寬或間距（`z_diff=90`，指定 `gap` 或 `width`），也可以反算某個線寬的阻抗。外層當成微帶線（參考往內的下一層銅，含防焊層），內層當成帶狀線（參考上下兩層）；用的是 KiCad 自己的傳輸線計算（和 pcb_calculator 相同）。參考層還沒鋪銅時會提醒。先用 `board_setup stackup="JLC04161H-7628"` 套 JLCPCB 的阻抗疊層，例如 50 Ω 在 F.Cu 是 0.376 mm、內層 0.297 mm；下單前請用 JLCPCB 的阻抗計算器核對。
- **`set_rules` 依阻抗寫網路類別：** `{"RF": {"impedance": 50}}` 設線寬，`{"USB": {"diff_impedance": 90}}` 設差分寬度與間距（有給 `diff_pair_gap` 就算寬度，有給 `diff_pair_width` 就算間距，都沒給就用類別的 clearance 當間距）；`impedance_layer` 指定以哪一層計算（預設 F.Cu），回應會列出其他層需要的線寬。
- **`auto_place` 連接器伸出板邊：** 板邊規則（`{"J1": "left"}`）會把 USB、RJ45、DC 插座等連接器轉成開口朝外。封裝有「PCB Edge」線（KiCad 庫約四分之一的 USB / RJ / DC 封裝有）就把那條線放在板邊，沒有就依外形判斷開口、前緣齊板邊；`{"side": "left", "overhang": 1}` 讓前緣再伸出 1 mm，`"inside": true` 保持在板內。焊盤會超出板邊的伸出量不會套用；旋轉規則把開口轉離板邊會被拒絕。回應的 `edge:` 行說明每個連接器怎麼放、伸出多少、依據什麼。
- **立創零件的 3D 模型：** `lcsc_fetch`、`add_footprint lcsc=` 會一併取得 EasyEDA 的 3D 模型，轉成 VRML 存到 `<專案>/lcsc.3dshapes/<封裝>.wrl`，封裝直接引用，3D 檢視器和 `pcb render` 都看得到。位置、旋轉、高度照 EasyEDA 的擺法（以 USB-C、SOT-23、SOT-223 驗證引腳都落在焊盤上）。每個模型第一次多一個請求，之後讀快取；`model=false` 可以不取。

**修正**
- **`sync_pcb` 會說出丟掉的製造屬性：** DNP、不進 BOM / 座標檔這些屬性以電路圖為準，只設在板子上的會在同步時被蓋掉。回應現在會列出來，並附上保留它們的 `sch_set_fields` 呼叫；它的 undo 也不再回報有零件移動。

## beta.1
第一個 beta。

**新工具（共 58 個）**
- **`sch_export_pdf`：電路圖 PDF。** 所有圖紙依頁序合成一份 PDF，有圖框與標題欄，圖紙之間可以點擊跳頁；點零件會跳出欄位選單（Adobe Acrobat / Reader；瀏覽器內建的 PDF 檢視器不支援）。用的是記憶體裡的電路圖，未存檔的修改也會出現在 PDF 裡。預設存成 `<專案>/<電路圖名>.pdf`，可選黑白。
- **`sch_annotate`：重新編號。** 和編輯器的 Annotate 相同，用的是 KiCad 自己的編號程式。`R?` 會拿到第一個空號，`reset=true` 依位置全部重編。範圍可以是全部、一張圖紙（含子圖紙）或指定零件；可設排序方向、起始號、每頁 100 / 1000 一段，也可以先 `dry_run`。整個動作是一筆 `sch_undo`，桌面版也能按 Ctrl+Z。之後跑 `sync_pcb`，板子上的零件會依 UUID 原地改名，不會移動。

**修正**
- **`sch_undo` 撤銷編號：** KiCad 比較符號時不看編號，所以只改了編號的修改，`sch_undo` 會以為已經撤銷過。現在比較時也會看各圖紙上的編號。
- **`open_board` 拒絕時說明原因：** 有未存檔的修改時，訊息會附上 `board_diff` 的結果。如果只是 undo 回存檔的狀態，訊息會寫「the same as the saved file」，表示 `discard=true` 不會丟掉任何東西。
- **刪除零件不再留下空網路：** `delete_footprints`、undo 新增、redo 刪除時，如果某個網路上已經沒有其他東西，會一起移除，回應會列出這些網路；undo 會把它們放回。以前刪掉 sync 進來的 R22 之後，網路數會比原本多。
- **Windows：** alpha.13 修好了 Windows 上路徑使用反斜線造成的「document … not open」。

**說明文件：** 新增 [docs/GUIDE.md](https://github.com/Brandon-git-hub/KiCadD/blob/main/docs/GUIDE.md)，內容包括安裝、接上 Agent、skill、10.99 存檔格式，以及做得到 / 做不到的一覽表。

## alpha.13
**`open_board` 換專案**
- 以前開另一個專案的板子一定失敗（「project … is already open」），重新開同一塊板也不會從檔案重新載入。現在會先關掉目前的專案，再開新的板子；同一個資料夾有電路圖的話會一起打開，網路類別等規則也換成新專案的。Agent 的 undo 歷史從頭開始。
- 目前的板子有未存檔的修改時會拒絕，要先 `save_board`，或加 `discard=true` 放棄修改。新檔案打不開時，會把原本的板子從檔案重新打開。桌面版裡一樣不能用，要由使用者在 pcbnew 開檔。

**alpha.12 試用（stickhub）回報的修正**
- **undo / redo 會重新灌銅：** 以前 fanout、delete_tracks、route 做完會重灌鋪銅，undo 卻不會，走線還原後碰到的是另一個狀態灌出來的鋪銅。stickhub 上 fanout 再 undo 之後，完整 DRC 有 3 個 GND 對 +1V8 / +5V 的間距錯誤（`set_rules` 回報的「DRC errors 3 -> 3」就是這個）。現在 undo / redo 只要動到走線、鋪銅或板框，就會自動重灌，回應寫「all zones refilled」。
- **undo 不再對沒檢查的東西說「drc: clean」：** undo 後的 DRC 只檢查被移動的零件附近。沒有零件移動時（例如 `set_rules`、`delete_tracks` 的 undo），以前會顯示「drc: clean」，現在改寫「drc: not checked here (no part moved; run_drc checks the board)」。
- **`board_diff` 會列出零件上文字的變化：** 編號、值和其他欄位在零件上的位置、角度、顯示或隱藏、所在層，例如「C3: reference moved -0.79, +0.94 mm on the part」。以前 `silk_tidy` 移動過的編號在 diff 裡看不到。
- **旋轉零件再 undo，文字角度會回到原狀：** 文字有「保持正向」時，KiCad 旋轉零件會把文字轉正，反向轉回去並不會回到原本的角度。這個差異以前看不到，`board_diff` 會比較文字之後才發現。現在移動、旋轉和 auto_place 的 undo 會存下每個欄位和文字的位置與角度。
- **`audit` 和 `silk_tidy` 的絲印判斷一致：** `silk_tidy` 判斷「超出板框」時用的是文字外框，外框比字大（含行距），所以把 stickhub 的 C3、U2 誤判成出界；`audit` 則根本不檢查板框。現在兩邊都用實際筆劃判斷出界，`audit` 也會列出超出板框的絲印文字，同一個零件自己的文字互相重疊也算。絲印沒有問題時，`audit` 會寫「silk: clean」，不再整段省略。`silk_tidy` 每一筆移動都會附上原因，例如「(was off the board)」。

## alpha.12
**`audit` 加上板子的檢查**
- **電流：** `currents {"VCC": 1.5}`（安培）會逐層找出每個網路最細的走線，和 IPC-2221 對這個電流、這一層銅厚要求的線寬比較，並寫出最細那段在哪裡、大約能過多少電流。網路類別的線寬不夠時，附上修改的 `set_rules` 呼叫。溫升預設 10 °C，可用 `temp_rise` 調整。
- **電壓間距：** `voltages {"VPP": 13}`（伏特）會逐層量出這個網路到其他網路最近的銅（走線、過孔、焊盤、鋪銅），依兩者的電壓差和 IPC-2221B 表 6-1 的要求比較（外層用 B2 無塗層，內層用 B1）。`+12V`、`3V3` 這類網路名稱會自動讀出電壓，其他網路當作 0 V。要求不超過最小網路類別間距的組合交給 DRC。每對網路只列一行，每個網路最多列 15 個最差的。請先 `refill_zones`。
- **絲印：** 列出壓到焊盤的絲印文字、互相重疊的零件編號，以及壓到焊盤的絲印線條（依零件彙整）。
- **測試點：** 列出沒有測試點（`TP*` 或 TestPoint 封裝）的網路。
- 沒有電路圖或還沒有板子時，`audit` 只跑有的那一半。

**新工具 `silk_tidy`：整理絲印編號（共 56 個工具）**
- 壓到焊盤、壓到其他絲印文字、或超出板框的零件編號，會移到零件旁最近的空位（上下左右、四角，再一圈圈往外；大零件最後可放在零件裡面）。文字方向跟著零件，橫的零件用橫字、直的零件用直字。
- 只移動有問題的編號；`all=true` 會重新放全部指定的零件，`values=true` 連可見的值一起整理，`dry_run=true` 只列出會怎麼移。一次 undo 就能還原。
- 判斷「壓到」用的是實際筆劃，不是文字外框，所以排得很密但沒有真的重疊的編號不會被動到。`audit` 的絲印檢查也改成同樣的判斷。

**新工具 `board_diff`：和上次存檔比較**
- 列出記憶體中的板子和上次存檔（或 `file` 指定的另一個 `.kicad_pcb`，例如舊版本）差在哪裡：零件的新增、刪除、移動、旋轉、換面、改編號、值、封裝、欄位、DNP / BOM，以及哪些焊盤換了網路；網路的增減；各網路的走線段數、長度和過孔數；銅箔區；板框、層數和圖形。
- 零件依 UUID 配對；另一個檔案的 UUID 不同時，零件依編號、走線和圖形依位置配對。只讀不改。每一類預設列 20 行（`limit`）。鋪銅和網路類別不比較。

**`set_rules` 依電流設定線寬**
- `{"classes": {"Power": {"current": 2}}}` 會依板子外層銅厚和 IPC-2221 算出線寬，寫進這個網路類別；有內層時，備註會寫出內層需要多寬。

**alpha.11 試用回報的修正**
- **`sch_render frame=true` 一定出整頁：** 以前同時給了 `refs` 或 `nets` 時會放大到零件那一帶，子圖紙上看起來就像沒有圖框和標題欄。現在 `frame=true` 優先，零件和網路照樣標示，但不放大。
- **`sch_render` 指定的區域沒有內容時會說明：** `area` 裡沒有任何圖元時，回應會寫「nothing drawn in this area」，並附上頁面大小；區域整個在頁面外也會註明。
- **`board_setup` 的板厚：** 套用 JLCPCB 疊層後，第一行直接寫「1.586 mm thick (the stackup's layers added up; nominal 1.6 mm)」，不再只寫 1.59 mm。
- **`lcsc_match` 認得公制封裝名稱：** `1005`、`1608`、`2012`、`3216` 這類公制尺寸（例如 `R_1005_C`、`1608_C`）以前被當成「不是貼片封裝」，現在分別對到 0402、0603、0805、1206。單獨寫 `0603` 仍視為英制；公制的 0603（英制 0201）要寫成 `0603Metric`。
- **`fanout` 放不下過孔時的說明：** 改成寫出第一個嘗試的位置，並提示可以用 `set_rules` 改小過孔。
- **工具清單更新：** MCP 的 `serverInfo.version` 以前固定是 0.2.0，現在是發行版本加上工具清單的雜湊（例如 `0.1.0-alpha.12+t54.7d4c2e59`）。工具或參數有變，版本就不同，依版本快取工具清單的客戶端會重新抓。如果還是看到舊清單，請在客戶端重新連線這個 MCP server（Claude Code 是 `/mcp`，Claude Desktop 要重新啟動）。

## 試用建議：貼片板（stickhub）
alpha.11 的試用板 `pic_programmer` 全是插件零件，`lcsc_match` 和 `fanout` 只能用臨時加上去的零件驗證。這一版請用 `share\kicad\demos\stickhub`（USB hub，94 個零件，除了一顆以外都是貼片，已佈線，有 GND / +5V / +1V8 鋪銅）試以下流程，順序不拘。不需要存檔，最後 undo 回原狀即可。
1. **`lcsc_match`**：先 dry run。預期 31 組裡有 10 組電阻電容配到料號（0402 / 0603 / 0805，封裝名稱是公制的 `1005_C`、`1608_C`、`2012_C`），其餘是 TVS、連接器、IC、晶振和 Logo，各附原因。`apply=true` 寫入，`sch_undo` 一次還原。22uF 10V 那組配到的料號在索引裡沒有寫耐壓，請在 JLCPCB 上確認。
2. **`fanout`**：先 `delete_tracks nets=["GND"]` 拿掉 GND 的走線，再 `fanout nets=["GND"] dry_run=true`。預期 87 個焊盤中約 67 個可以打過孔接到另一面的 GND 鋪銅，其餘會寫「no room for a via」和位置。實際執行後跑 `run_drc`，再用 `undo` 退回兩步。
3. **`audit`**：試試 `currents {"+5V": 2}` 和 `voltages`，看板子那一半的結果（線寬、間距、絲印、測試點）。`+5V`、`+1V8` 這類網路會從名稱讀出電壓。
4. **`set_rules`** `classes {"Power": {"current": 2}}`，看算出的線寬；`undo`。
5. **`silk_tidy dry_run=true`**，然後實際執行，用 `board_render layers=["silk"]` 看結果；`undo`。
6. **`board_diff`**：每一步做完都可以叫一次，看它列出的變更是否和你做的一致；全部 undo 後應該是「no changes」。

這個流程在 Linux 上用 `client/smd_e2e.py` 自動跑過（離線，不查即時庫存）。

## alpha.11
**新功能：`sch_render` 電路圖出圖（共 54 個工具）**
- **出一張圖紙的 PNG 給 Agent 看：** 用 KiCad 自己的 plot（符號、導線、標籤、欄位），可以看出零件或文字重疊、導線交叉、標籤離開導線、整張圖好不好讀。上方和左邊有 mm 尺規，座標和 `sch_summary` 一致。
- **`refs`：** 框出並標示零件，圖會放大到它們那一帶，並自動切到第一顆零件所在的圖紙。
- **`nets`：** 把網路的導線、標籤和腳位畫成藍色，階層圖紙裡的也算。
- **其他參數：** `sheet` 指定圖紙（名稱或路徑），`area` 指定區域，`frame=true` 出整頁（含圖框和標題欄），`path` 另外存檔。
- **指定的零件或網路不在這張圖紙上時：** 回應會說它在哪一張。

**改名：`render_board` → `board_render`**
- 和 `board_summary` / `sch_summary` 一樣，PCB 的工具以 `board_` 開頭，電路圖的工具以 `sch_` 開頭，所以出圖工具改成 `board_render` / `sch_render` 一對。舊名稱不保留，呼叫時會回「unknown tool」。

**alpha.10 試用回報的修正**
- **認不得的參數改成報錯：** 以前打錯或猜錯的參數名稱會被忽略。例如 `zones create` 給了 `area`，結果建出一塊全板大小的銅箔區。現在所有工具遇到不認得的參數（包含 `create` 裡面的）都會回錯誤，並列出可用的參數。`zones create` 也新增了 `rect: [x0, y0, x1, y1]`。
- **`add_footprint like=` 複製出的零件不再連到電路圖：** 以前複製時連電路圖的連結也一起帶過去，`pcb_set_fields` 因此誤報「這顆來自電路圖」，`sync_pcb` 也可能把複製品當成原本那顆。現在複製品就是手動放的零件。
- **`add_footprint` 的 undo 會移除它新建的網路：** 沒有其他東西用到的網路才會移除，和 `sync_pcb` 的 undo 一樣。
- **`board_render layers=["silk"]` 不再畫銅：** 改成畫阻焊開窗（看得出焊盤位置）、絲印和板框。層名稱改用標準名稱，板子有自訂名稱時寫在括號裡，例如 `F.Cu (top_layer)`。
- **`lcsc_match` 的庫存寫清楚：** 分成「JLCPCB 組裝庫存（索引日期）」、「JLCPCB 現在能不能組裝」和「LCSC 零售庫存」三項。原本的「(121 in stock)」其實是符合條件的料號數量，現在寫成「first of 121 matching parts」。LCSC 零售缺貨但 JLCPCB 能組裝的基礎件，不再被跳過。
- **疊層板厚：** 套用 JLCPCB 疊層後，回應會說明板厚是 JLCPCB 表上各層的加總：4 層在 1.45 到 1.65 mm 之間，6 層在 1.52 到 1.59 mm 之間，標稱都是 1.6 mm。

## alpha.10
**新功能（4 個新工具，共 53 個）**
- **`export_fab` 先跑 DRC：** 鋪銅後跑完整 DRC，錯誤數（依類別）列在回應最前面。`strict=true` 時，只要 DRC 有錯、有重複編號或零件在板框外，就一個檔案都不寫並說明原因；`drc=false` 可以跳過 DRC。
- **`lcsc_match` 批次配對立創料號：** 電路圖裡還沒有 LCSC 編號的電阻、電容、電感、磁珠、LED（貼片封裝 0402、0603 等），同值同封裝的歸成一組，從本機料號索引裡選一顆（基礎件優先，再來是優選件，最後依庫存），並即時查一次今天的庫存，每組約 1 秒。先 dry run 列出結果，`apply=true` 才一次寫入（一次 `sch_undo` 就能還原）。`pick {"R1": "C25804"}` 可以為某一組指定別的料號，也可以補上沒配到的組。值裡可以寫規格，例如 `"100nF 50V X7R"`、`"10k 1%"`；LED 要在值裡寫顏色。配不到的會說明原因（插件封裝、沒有封裝、不是被動元件等）。
- **`render_board` 出圖給 Agent 看：** 用 KiCad 自己的 plot 畫出 PNG（銅、焊盤、鋪銅、絲印等），預設是正面全板。`refs` 會框出並標示零件，`nets` 用青色畫出網路；給了其中之一時，圖會放大到那一帶。也可用 `area` 指定區域，`back=true` 從背面看，`layers` 可選 `silk`、`fab`、`copper` 或指定層名。上方和左邊有 mm 尺規，飛線以白線顯示；`path` 可另外存檔。用來看絲印重疊、零件方向和擁擠程度。
- **`stitch_vias` 縫合過孔：** 依格點（預設 5 mm）在某個網路（預設 GND）的鋪銅裡打過孔，只放在兩層以上都有該網路銅的位置。過孔會避開焊盤、走線、孔、板邊、禁止區，也不放在零件的 courtyard 裡。格點放完後，還沒有過孔的銅島會各補一個。執行前後都會重新鋪銅，一次 undo 就能還原。在 pic_programmer 上（雙面 GND）打了 213 個，DRC 0 錯誤。
- **`teardrops` 淚滴：** 用 KiCad 自己的淚滴，加在過孔和插件焊盤上；`smd=true` 時也加在貼片焊盤上，`remove=true` 移除。之後移動零件或走線時會自動跟著更新，和在編輯器裡一樣。一次 undo 就能還原。
- **`board_setup stackup="JLC04161H-7628"`：** 套用 JLCPCB 的阻抗疊層：18 種 4 層、8 種 6 層，標稱 1.6 mm。每一層 prepreg / core 的厚度和介電常數都照官方頁面填入，`stackup="list"` 可列出名稱。介電常數取自 jlcpcb.com/impedance（2026-11-14）；JLCPCB 的下單頁寫的數值不同，計算控制阻抗的線寬前，請先用 JLCPCB 的阻抗計算器核對。

**alpha.9 試用回報的修正**
- **`zones` 的工具說明：** 說明現在先講怎麼新建銅箔區（附兩個例子），`create` 底下的每個參數也都列出來了。alpha.9 的說明只寫了列出和修改。
- **`pcb_set_fields` 可以改編號：** `{"R22": {"Reference": "R23"}}`。新編號不能和其他零件重複；板子上有兩顆同編號時，改的是手動放的那一顆。零件來自電路圖時會提醒：下次 `sync_pcb` 會把編號改回來，要保留就改電路圖。`sync_pcb` 的重複編號警告現在給的就是這個呼叫。
- **`board_outline` 當場提醒衝突：** 新固定孔的位置已經有孔時不再重複放（回應會說是哪一顆零件的孔）；孔壓到零件或穿過走線、挖槽切到走線、新板框把走線留在板外，都直接列在回應裡，不用等跑 DRC。挖槽上的零件也算進「不在板框內」。
- **`zones create` 處理重疊：** 新銅箔區落在另一個網路、較大的銅箔區裡面時（例如 GND 裡的一塊 +3V3），自動排在它上面一級。和另一個網路的銅箔區重疊、優先順序又相同時會警告，並附上調整優先順序的呼叫。
- **undo 後提醒重新鋪銅：** undo / redo 動到銅箔區、板框或層數時，回應會註明銅箔還是舊的，`unconnected` 要等 `refill_zones` 之後才準。
- **新網路的網路類別：** `add_footprint` 建立的新網路會立刻套用依名稱指定的網路類別。之前要重新載入板子才生效，這段期間 `route` 會用 Default 的線寬和間距，DRC 卻用指定的類別來檢查。

## alpha.9
**從空白專案開始的新板子（47 → 49 個工具）**
- **`board_outline`（新）：** 畫板框，可以是矩形、圓角矩形或圓形，原本的板框會被取代。`cutouts` 在板內挖槽或挖圓孔；`hole_inset=3.5` 在四個角各放一個 M3 固定孔（H1～H4，不進 BOM 和 CPL），`holes` 放在指定位置，也可以做成接 GND 的鍍銅孔。回應附尺寸、面積、還在板外的零件。可以 undo。
- **`board_setup`（新）：** 查看或設定層數（2、4、6…）、板厚、內外層銅厚。疊層會依 KiCad 預設的方式重新產生。要減少層數時，被移除的內層必須是空的。可以 undo。
- **`zones create`：** 新建銅箔區，例如 `{"net": "GND"}` 會依板框在 F.Cu 和 B.Cu 鋪 GND，或用 `layers: ["In1.Cu"]` 做內層的地。建好後自動鋪銅，可以 undo。
- **`set_rules preset`：** `jlcpcb_2layer`、`jlcpcb_4layer`、`jlcpcb_6layer`，一次把板子的最小線寬、間距、孔徑、環寬、銅到板邊距離、絲印字高設成 JLCPCB 不加價的標準製程，Default 網路類別也一起調到不低於這些值。數值依 JLCPCB 官方製程能力頁（2026-11 查閱），下單前請再核對。

**alpha.8 試用回報的修正**
- **`sync_pcb` 可以 undo：** 整次同步是一筆 Agent undo（新增、刪除、改值或欄位的零件、換網路的走線都會還原，同步新建、undo 後沒用到的網路也會移除），`redo` 可再做一次。
- **重複編號：** 板子上已經有一顆手動放的 R22（`add_footprint`，沒有連到電路圖），電路圖又加了 R22 時，`sync_pcb`（含 dry run）會先警告「會再加一顆 R22」。`run_drc` 和 `export_fab` 也會提醒同一個編號用在多顆零件上。
- **`export_fab` 提醒板外的零件：** 不在板框內、但仍會寫進 BOM 和 CPL 的零件，列在最前面。
- **`add_footprint lcsc=`：** 被動元件的 Value 改用規格值（`10k`），和符號端一致，不再是料號。
- **從焊盤內任一點出線：** `route from=[x, y]` 指在焊盤內、不在中心的點時，從那一點出線（兩個焊盤很近時，例如焊接跳線）。焊盤旁太擠佈不出去時，會提示改用較細的線寬或焊盤內的出線點。
- **終點在另一層的備案：** `route` 不給終點、或 `route_short` 時，最近的目標（例如另一層的走線）佈不到，會改試同一組已接上的焊盤和過孔（由近到遠）。
- **回應更清楚：** 接到走線或過孔時附上座標（`track VCC (B.Cu) @145.1,97.47`）；換層的線依序列出各層（`routed on F.Cu -> B.Cu`）；`fanout` 沒有銅箔區的網路合併成一行（`no copper zone of VCC ...; skipped R22.1, JP1.1`）。

## alpha.8
**佈線（44 → 47 個工具）**：代替手動重複的部分，不是全板自動佈線。走線的畫法和 KiCad 的互動式佈線器相同（用 KiCad 自己的推擠佈線器，繞開障礙、不推動既有走線，線寬和過孔依網路類別，遵守間距、禁止佈線區、板邊距離）；要接哪裡、在哪裡換層由 Agent 或你決定。每次呼叫是一筆 undo，你也可以在編輯器裡按 Ctrl+Z。
- **`route`**：連一條線。`from="U1.5"` 不給 `to` 時接最近的未佈線連線，或 `to="C3.1"` / 座標；`waypoints` 指定經過點，`[x, y, "B.Cu"]` 在該點打過孔換層，`via: [x, y]` 是只換一次層的簡寫。回應附長度與直線距離的比值、線段數、過孔數、最窄的間距（和什麼）；佈不通時說明走到哪裡、被哪條線或哪個焊盤擋住。
- **`route_short`**：把指定零件或網路的未佈線連線由短到長逐條佈（短的最不容易擋到別人），`max_length` 只佈短的，`dry_run` 先看哪些會失敗。整次呼叫一筆 undo。
- **`fanout`**：把 SMD 焊盤用短線加過孔接到另一層同網路的銅箔區（內層的地，或雙層板背面的地）。通孔焊盤、已經接上的焊盤直接跳過；目標層沒有這個網路的銅箔區時只警告，不打空接的過孔；過孔一定落在鋪好的銅裡；完成後自動重新鋪銅。
- 孔對孔距離、銅碰到挖孔這兩項佈線器本身不管，另外檢查，違反的那條不會畫上。

**alpha.7 試用回報的修正**
- **立創被動元件的腳位：** EasyEDA 常把電阻、電容的腳標成 input，現在一律改成 passive（已經存進專案 LCSC 庫的舊符號要加 `refresh=true` 重抓一次）。Value 改用規格值（`10k`、`100nF`），不再是料號。
- **`get_rules` 的網路數：** 新增的網路類別只設了部分數值時，Default 的網路數沒有扣掉，加總會比總數多。
- **`sync_pcb dry_run`：** warning 排在最前面、不會被截掉；不再每顆零件都列「Update X fields」（那是 KiCadD 自己清除舊 LCSC 編號造成的）；dry run 寫「board has N footprints (unchanged: dry run)」；`detail=true` 列出全部。
- **`add_footprint`：** 第一行就是新零件的編號（`C11 added @..., from copy of C10`），`batch` 裡也看得到。
- **層名稱：** 一律用 KiCad 的標準名稱（`F.Cu`、`B.Cu`），不再用板子自訂的名稱（例如 `top_layer`）。
- **`export_fab`：** 備註也列出被排除的 DNP 零件；`sch_set_fields` / `pcb_set_fields` 的說明註明找不到的編號會列出、其他零件照改。

## alpha.7
**新工具（36 → 44 個）**
- **電路圖的 Agent undo / redo**：`sch_undo` / `sch_redo` 撤銷 Agent 自己最近的電路圖修改（保留 50 步）；你之後改過的東西不動，你已經按 Ctrl+Z 撤銷的會直接說明。
- **改欄位**：`sch_set_fields`（電路圖：值、封裝、LCSC、自訂欄位、DNP、不列入 BOM 等，可一次多顆）、`pcb_set_fields`（板子上的封裝欄位與屬性）。`sch_part` 會列出自訂欄位與屬性。
- **設計規則**：`get_rules` / `set_rules`（網路類別的線寬、間距、過孔、差分對；依網路名稱指派類別；板子的最小值）。改完自動跑 DRC 回報錯誤數的變化，可 undo，存檔時寫進專案檔。
- **銅箔區**：`zones` 列出或修改銅箔區的間距、最小寬度、優先順序、焊盤連接方式（實心 / 散熱焊盤），改完自動重新鋪銅，可 undo。
- **批次**：`batch` 一次呼叫依序跑多個工具（可重複、遇錯停止），每步只回一行。

**精簡輸出**：`undo` / `redo` / `move_footprints` / `add_footprint` / `delete_*` 超過 8 顆零件時只列前 5 顆，局部 DRC 超過 10 筆時附分類統計；`auto_place` 只列前 15 個零件編號。`detail=true` 列出全部。

**建議的使用方式（skill）**：release 附上 `kicadd-skill.zip`（zip 裡也有 `share\kicadd\skills\kicadd`），教 Agent 怎麼搭配這些工具，安裝方式見下方「接上 Agent」。

**修正**
- **新做的封裝馬上能用：** `lib_create_footprint` 或 `lib_register` 加進來的封裝庫，`add_footprint` 之前要等一陣子才找得到，現在立即可用。
- **CPL 與 BOM 一致：** 「不列入 BOM」的零件（例如固定孔）不再出現在 CPL，上傳 JLCPCB 時兩份檔案對得上。
- **刪除導線：** `sch_delete wires=[[x, y]]` 刪掉經過那一點的導線；`dangling=true` 清掉所有一端懸空的線。`sch_disconnect` 會刪整條線，不再留下懸空的一段。
- **封裝檢查：** `lib_create_symbol` / `sch_add_part` 指定的封裝不存在時會提醒。
- **其他：** 立創被動元件的腳位改為 passive；`sync_pcb` dry run 不再以錯誤回傳、報告過長時註明省略幾行；「編輯器忙碌中」會說明原因；`save_board` / `sch_save` 的說明註明會升級檔案格式。

## alpha.6
- **反覆擺件 / undo 時記憶體一直增加：** 編輯器的 Ctrl+Z 歷史每一步都存一份改到的零件，而 KiCad 的復原步數上限設定（`max_undo_items`）因為一個上游的 bug 從來沒有生效，歷史會無限增加。已修正，預設保留 100 步（`%APPDATA%\kicad\10.99\pcbnew.json` 的 `system.max_undo_items`：0 = 100 步，負數 = 不限）。
- **`run_drc`：** 用 `types` / `refs` 篩選時，另外列出符合篩選的數量（上面的類別統計是全板的）。
- **`audit`：** 固定孔、光學定位點、測試點不再算成連接器（`pic_programmer` 從 10 個變成 4 個）。

## alpha.5
**修正**
- **桌面版當掉：** Agent 新增零件 → Agent `undo` → 你在編輯器按 Ctrl+Z，編輯器會當掉（Agent 的 undo 刪掉了編輯器復原紀錄還在用的零件）。已修正。
- **BOM 用錯立創編號：** 從立創抓的封裝存進專案庫時帶著零件編號，另一顆用同一封裝的零件（例如 1k 電阻用了 10k 抓來的 `R0603`）會以錯的編號進 BOM。現在庫裡的封裝不帶編號，`sync_pcb` 也會清掉舊庫留下的編號。
- **`auto_place` 比原本差就不套用：** 板上零件都已擺好時，結果不比目前好就不移動（`force=true` 仍可套用），並提醒擺完要先 `refill_zones` 再 `run_drc`。
- **你在編輯器 Ctrl+Z 撤銷 Agent 的修改後，Agent 的 `undo`** 只回一行「已在編輯器裡撤銷」，不再逐一列出零件。
- **ERC / DRC 不再對 KiCad 官方庫逐一警告**（本版沒附那些庫）：`pic_programmer` 的 ERC 從 64 個警告剩 3 個真的問題。

**新工具（26 → 36 個）**
- **電路圖**：`sch_move`（移動 / 旋轉零件，連線跟著走）、`sch_wire`（畫導線，自動加 junction）、電源符號（`sch_connect power: ["GND", "+3V3"]` 或 `sch_add_part lib_id="power:GND"`，不需要電源庫）、`run_erc`（未存檔的修改也查）。
- **審查**：`audit`：去耦電容（在板上量到 IC 腳的距離）、電源來源、對外連接器的 ESD 保護、只有一個腳位的網路。
- **製造**：`export_fab`：Gerber、鑽孔、上傳用 zip、JLCPCB 格式的 BOM 與 CPL，直接用編輯器裡的板子（未存檔也行）。
- **依規格找立創零件**：`lcsc_search`，例如 `value "100nF", package "0402", dielectric "X5R", voltage_min 16`。資料來自 jlcparts 每日更新的 JLCPCB 型錄（有庫存的約 55 萬筆），第一次使用時下載約 45 MB、每週更新；前幾筆會再即時查 LCSC 庫存。
- **PCB**：`delete_tracks`（刪全部 / 某網路 / 某區域 / 某零件的走線，可 undo）。
- **元件庫**：`lib_create_symbol`（依腳位清單產生方框符號）、`lib_create_footprint`（chip、SOT-23、SOIC/SO、QFN、QFP、排針）、`lib_register`（把磁碟上現有的庫加進專案）。

## alpha.4
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

### 建議的使用方式（skill）
58 個工具怎麼搭配使用（先查再改、新板設定、審查 / 擺件 / 佈線 / 找料 / 出廠流程、undo、批次、常見陷阱）寫成一份 skill，讓 Agent 照著做：
- **Claude Code**：把 `share\kicadd\skills\kicadd` 整個資料夾複製到 `%USERPROFILE%\.claude\skills\`（所有專案）或專案的 `.claude\skills\`。
- **Claude Desktop**：在設定的 Skills 頁面上傳另外附的 `kicadd-skill.zip`。
- **其他 Agent（Cursor 等）**：把 `SKILL.md` 的內容放進它的規則檔或系統提示。

## Agent 能做的事（58 個 MCP 工具）
- **電路圖**：`sch_add_part`（符號庫、立創編號或電源符號）、`sch_connect` / `sch_disconnect`（網路名稱連接）、`sch_move`、`sch_wire`、`sch_netlist`、`sch_delete`（零件、導線、懸空線）、`sch_set_fields`、`run_erc`、`sch_render`（PNG 圖）、`sch_export_pdf`（全部圖紙一份 PDF）、`sch_annotate`（重新編號）、`sch_save`、Agent 自己的 `sch_undo` / `sch_redo`；**階層圖紙** `sch_add_sheet`，`ports` 產生圖紙腳位，父圖紙上用 `圖紙名.腳位名` 連接。
- **立創零件**：`lcsc_fetch` / `sch_add_part lcsc=` 依編號抓封裝與符號，存進專案的 LCSC 庫；回報庫存、價格、基礎件 / 擴展件。編輯器裡也有 Place → Add LCSC Part / Symbol。
- **電路圖 → PCB**：`sync_pcb`（開著電路圖編輯器時直接讀編輯器內容，不會寫入你的電路圖檔）。
- **自動擺件** `auto_place`：依連線擺放、去耦電容靠 IC 腳；選用功能：模組分群（`groups` 或 Block 欄位）、去耦電容放背面（`back_decaps`）、擺件規則（`rules`：板邊、區域、角度、距離上限）。
- **PCB 編輯**：查詢、移動、翻面、新增 / 刪除封裝、改欄位（`pcb_set_fields`）、刪除走線、DRC、鋪銅、存檔、Agent 自己的 undo / redo。
- **佈線**：`route`（一條線，可指定經過點與換層位置）、`route_short`（短連線由短到長）、`fanout`（SMD 焊盤打過孔接到另一層的銅箔區）、`stitch_vias`（縫合過孔）、`teardrops`（淚滴）。不是全板自動佈線。
- **新板設定**：`board_outline`（板框、挖槽、固定孔）、`board_setup`（層數、板厚、銅厚、疊層，或 JLCPCB 的阻抗疊層）、`set_rules preset`（JLCPCB 製程預設）、`zones create`（新建銅箔區）。
- **設計規則與銅箔區**：`get_rules` / `set_rules`（網路類別、指派、板子最小值）、`zones`（間距、優先順序、焊盤連接）。
- **批次**：`batch` 一次跑多個工具。
- **審查與製造**：`audit`（電路圖與板子：去耦、電源、ESD、電流對線寬、電壓對間距、絲印、測試點）、`board_diff`（和上次存檔比較）、`silk_tidy`（整理絲印編號）、`board_render`（PNG 圖）、`export_fab`（先跑 DRC，再輸出 Gerber / 鑽孔 / BOM / CPL）；依規格找料 `lcsc_search`、批次配料號 `lcsc_match`；自建符號與封裝 `lib_create_symbol` / `lib_create_footprint`。
- **與你協同**：Agent 的修改就是編輯器的提交，你按 Ctrl+Z 就能撤銷；你在互動工具中（走線、畫線、對話框）時，Agent 的修改會被擋下，不會干擾你。走線由你來做。
- **怎麼分工最有效**（試用心得）：了解設計、審查、擺件、局部修改用 Agent；大量欄位修改可以用 `sch_set_fields` / `batch`，或在 KiCad 關閉時直接改檔再用 `kicad-cli` 驗證；DRC / ERC / 匯出等批次工作用 `kicad-cli` 或腳本。

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
- 尚未支援：全板自動佈線、差分對 / 等長的自動佈線、PCB / 組裝報價、立創零件的 3D 模型、同一圖紙檔重複使用、連接器伸出板邊的擺放。
- 問題回報：Help → Report Bug（開 GitHub issue）。

## CI 驗證（smoke test）
`kicad-cli` 啟動、`pic_programmer` DRC、`rectifier` 匯出 SPICE netlist、API server 經 `kicadd-mcp --stdio` 回應 `board_summary`。
