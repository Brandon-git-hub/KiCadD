<!-- Release notes template for tag builds (.github/workflows/windows.yml, job release).
     {{UPSTREAM}} is filled from patches/UPSTREAM_BASE; build info and SHA-256 are appended. -->
KiCadD 試用版：以 KiCad nightly（upstream {{UPSTREAM}}）為基礎的修剪版 Windows x64 build，加上 Agent（MCP）協同設計。

> ⚠️ Pre-release，僅供試用。由 GitHub Actions 自動建置；功能在 Linux 上做過端到端測試，Windows 上只有 CI 的 smoke test 與早期版本的人工試用。

## 這一版（alpha.10）
**新功能（4 個新工具，共 53 個）**
- **`export_fab` 先跑 DRC：** 鋪銅後跑完整 DRC，錯誤數（依類別）列在回應最前面。`strict=true` 時，只要 DRC 有錯、有重複編號或零件在板框外，就一個檔案都不寫並說明原因；`drc=false` 可以跳過 DRC。
- **`lcsc_match` 批次配對立創料號：** 電路圖裡還沒有 LCSC 編號的電阻、電容、電感、磁珠、LED（貼片封裝 0402、0603 等），同值同封裝的歸成一組，從本機料號索引裡選一顆（基礎件優先，再來是優選件，最後依庫存），並即時查一次今天的庫存，每組約 1 秒。先 dry run 列出結果，`apply=true` 才一次寫入（一次 `sch_undo` 就能還原）。`pick {"R1": "C25804"}` 可以為某一組指定別的料號，也可以補上沒配到的組。值裡可以寫規格，例如 `"100nF 50V X7R"`、`"10k 1%"`；LED 要在值裡寫顏色。配不到的會說明原因（插件封裝、沒有封裝、不是被動元件等）。
- **`render_board` 出圖給 Agent 看：** 用 KiCad 自己的 plot 畫出 PNG（銅、焊盤、鋪銅、絲印等），預設是正面全板。`refs` 會框出並標示零件，`nets` 用青色畫出網路；給了其中之一時，圖會放大到那一帶。也可用 `area` 指定區域，`back=true` 從背面看，`layers` 可選 `silk`、`fab`、`copper` 或指定層名。上方和左邊有 mm 尺規，飛線以白線顯示；`path` 可另外存檔。用來看絲印重疊、零件方向和擁擠程度。
- **`stitch_vias` 縫合過孔：** 依格點（預設 5 mm）在某個網路（預設 GND）的鋪銅裡打過孔，只放在兩層以上都有該網路銅的位置。過孔會避開焊盤、走線、孔、板邊、禁止區，也不放在零件的 courtyard 裡。格點放完後，還沒有過孔的銅島會各補一個。執行前後都會重新鋪銅，一次 undo 就能還原。在 pic_programmer 上（雙面 GND）打了 213 個，DRC 0 錯誤。
- **`teardrops` 淚滴：** 用 KiCad 自己的淚滴，加在過孔和插件焊盤上；`smd=true` 時也加在貼片焊盤上，`remove=true` 移除。之後移動零件或走線時會自動跟著更新，和在編輯器裡一樣。一次 undo 就能還原。
- **`board_setup stackup="JLC04161H-7628"`：** 套用 JLCPCB 的阻抗疊層：18 種 4 層、8 種 6 層，都是 1.6 mm。每一層 prepreg / core 的厚度和介電常數都照官方頁面填入，`stackup="list"` 可列出名稱。介電常數取自 jlcpcb.com/impedance（2026-11-14）；JLCPCB 的下單頁寫的數值不同，計算控制阻抗的線寬前，請先用 JLCPCB 的阻抗計算器核對。

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
53 個工具怎麼搭配使用（先查再改、新板設定、審查 / 擺件 / 佈線 / 找料 / 出廠流程、undo、批次、常見陷阱）寫成一份 skill，讓 Agent 照著做：
- **Claude Code**：把 `share\kicadd\skills\kicadd` 整個資料夾複製到 `%USERPROFILE%\.claude\skills\`（所有專案）或專案的 `.claude\skills\`。
- **Claude Desktop**：在設定的 Skills 頁面上傳另外附的 `kicadd-skill.zip`。
- **其他 Agent（Cursor 等）**：把 `SKILL.md` 的內容放進它的規則檔或系統提示。

## Agent 能做的事（53 個 MCP 工具）
- **電路圖**：`sch_add_part`（符號庫、立創編號或電源符號）、`sch_connect` / `sch_disconnect`（網路名稱連接）、`sch_move`、`sch_wire`、`sch_netlist`、`sch_delete`（零件、導線、懸空線）、`sch_set_fields`、`run_erc`、`sch_save`、Agent 自己的 `sch_undo` / `sch_redo`；**階層圖紙** `sch_add_sheet`，`ports` 產生圖紙腳位，父圖紙上用 `圖紙名.腳位名` 連接。
- **立創零件**：`lcsc_fetch` / `sch_add_part lcsc=` 依編號抓封裝與符號，存進專案的 LCSC 庫；回報庫存、價格、基礎件 / 擴展件。編輯器裡也有 Place → Add LCSC Part / Symbol。
- **電路圖 → PCB**：`sync_pcb`（開著電路圖編輯器時直接讀編輯器內容，不會寫入你的電路圖檔）。
- **自動擺件** `auto_place`：依連線擺放、去耦電容靠 IC 腳；選用功能：模組分群（`groups` 或 Block 欄位）、去耦電容放背面（`back_decaps`）、擺件規則（`rules`：板邊、區域、角度、距離上限）。
- **PCB 編輯**：查詢、移動、翻面、新增 / 刪除封裝、改欄位（`pcb_set_fields`）、刪除走線、DRC、鋪銅、存檔、Agent 自己的 undo / redo。
- **佈線**：`route`（一條線，可指定經過點與換層位置）、`route_short`（短連線由短到長）、`fanout`（SMD 焊盤打過孔接到另一層的銅箔區）、`stitch_vias`（縫合過孔）、`teardrops`（淚滴）。不是全板自動佈線。
- **新板設定**：`board_outline`（板框、挖槽、固定孔）、`board_setup`（層數、板厚、銅厚、疊層，或 JLCPCB 的阻抗疊層）、`set_rules preset`（JLCPCB 製程預設）、`zones create`（新建銅箔區）。
- **設計規則與銅箔區**：`get_rules` / `set_rules`（網路類別、指派、板子最小值）、`zones`（間距、優先順序、焊盤連接）。
- **批次**：`batch` 一次跑多個工具。
- **審查與製造**：`audit`、`render_board`（PNG 圖）、`export_fab`（先跑 DRC，再輸出 Gerber / 鑽孔 / BOM / CPL）；依規格找料 `lcsc_search`、批次配料號 `lcsc_match`；自建符號與封裝 `lib_create_symbol` / `lib_create_footprint`。
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
