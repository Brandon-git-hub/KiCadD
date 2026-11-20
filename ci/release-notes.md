<!-- Release notes template for tag builds (.github/workflows/windows.yml, job release).
     {{UPSTREAM}} is filled from patches/UPSTREAM_BASE; build info and SHA-256 are appended. -->
KiCadD 試用版：以 KiCad nightly（upstream {{UPSTREAM}}）為基礎的修剪版 Windows x64 build，加上 Agent（MCP）協同設計。

> ⚠️ Beta（pre-release）。已能走完整個設計流程，功能仍會持續加入。由 GitHub Actions 自動建置；30 個端到端測試在 Linux 上全部通過（含桌面版），Windows CI 跑 smoke test 與 headless 的出廠、貼片、電路圖文件、工具、設定、undo 歷史測試；桌面版在 Windows 上只有人工試用。

## 使用前須知
完整的使用說明（安裝、接上 Agent、skill、存檔格式、做得到 / 做不到）：[docs/GUIDE.md](https://github.com/Brandon-git-hub/KiCadD/blob/main/docs/GUIDE.md)。

- **存檔格式：** KiCadD 以 KiCad nightly（10.99）為基礎。用 KiCadD 存過的 `.kicad_pcb`、`.kicad_sch` 會寫成新格式，**KiCad 9 / 10 正式版可能打不開**。請用專案副本試用；要知道這次改了什麼，存檔前先用 `board_diff`。
- **平台：** 只提供 Windows x64。Linux 可以自行從原始碼編譯，沒有 macOS 版。
- **不附 KiCad 官方元件庫：** 符號和封裝請用 `lcsc_fetch`、`lcsc_match` 從 LCSC 取得，或用 `lib_create_symbol`、`lib_create_footprint` 自建，也可以 `lib_register` 專案裡現有的庫。
- **一次一個專案：** 一個服務同時只開一個專案（板子加電路圖），換專案請用 `open_board`。
- **目前不支援：** 差分對與等長佈線、全板自動佈線（`route` 只代替重複的手動佈線）、電路圖自動排版、組裝圖、STEP 匯出（3D 模型只有 VRML）、ODB++ / IPC-2581 / DXF、MCP resources / prompts。

## 這一版（beta.4）
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
