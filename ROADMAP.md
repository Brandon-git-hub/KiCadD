# KiCadD 路線圖（2026-11-18 定稿）

起點：`0.1.0-alpha.13`。Linux 上 24 套 e2e 全部通過（`client/run_all.py`）；Windows CI 跑 smoke、fab e2e、SMD e2e。

## 原則

1. **維持精簡架構。** 不加回 OpenCASCADE，不恢復 wxWidgets 模擬視窗（保留 SPICE 網表匯出）。
2. **不建靜態模型庫、不寫爬蟲。** 電氣參數只從現有的 `jlcparts` 索引抽取。
3. **不做自動佈線、不做自動等長繞線（meander）。** Headless 的產出是擺件；推擠、差分對、等長由人在桌面版用 KiCad 原生工具完成。Agent 端只做**檢查**。
4. **先做確定性的 Layout-Aware 檢查，再做模擬。** 前者只用已有的板子幾何、疊層與銅厚，不需新相依套件；模擬依賴模型品質，開工前先量參數覆蓋率。
5. **新增的模擬後端以外部執行檔呼叫**（`ngspice`），不連結進精簡 build。

## 階段零：Beta ✅（2026-11-18，v0.1.0-beta.1）

Beta 已能走完整個設計流程（電路圖 → 選料 → 擺件 → 規則 → 出廠檔）。之後仍會持續加入功能。

1. **電路圖 PDF 匯出**（MCP 工具，沿用上游的 PDF plotter）。
2. **自動重新編號 Annotate**（MCP 工具，沿用上游的 annotate；含桌面版共用 undo）。
3. **Windows 測試補齊。** headless 的 `tools`、`setup`、`history` 等加進 Windows CI；桌面版 e2e（`desktop`、`desktop_sch`、`desktop_autoplace`）在 Windows 上至少手動跑過一次，能自動化就自動化。
4. **open_board 的「未存檔」誤報。** undo 回原狀後仍拒絕：先在拒絕訊息附上 board_diff 結果（內容與檔案相同時說明 `discard=true` 不會丟東西）；真正清除修改旗標列入後續 TODO。 ✅ 拒絕訊息已附 board_diff（2026-11-18）。
5. **技術債：** 刪除封裝後殘留的空網路等小問題。 ✅ 刪除封裝（及 undo 新增、redo 刪除）時，沒有其他東西在上面的網路一併移除，undo 會放回（2026-11-18）。
6. **Beta 說明文件：** 安裝、skill 用法、10.99 檔案格式限制、「做得到 / 做不到」一覽。 ✅ [docs/GUIDE.md](docs/GUIDE.md)（2026-11-18）。

## 階段一：交付閉環（beta.x） ✅（2026-11-19）

依成本由低到高：

1. **控制阻抗線寬計算。** 重用 pcb_calculator 的 transline 計算與內建 26 種 JLCPCB 疊層，估算單端 / 差分線寬並寫入規則。 ✅ `impedance` 工具（各銅層的線寬 / 間距，或反算阻抗）與 `set_rules` 的 `impedance` / `diff_impedance`（寫入網路類別），用 KiCad 的 `common/transline_calculations`（2026-11-18）。
2. **`auto_place` 連接器伸出板邊。** USB-C、RJ45、DC Jack 等依規則突出板框。 ✅ 板邊規則把連接器轉成開口朝外：封裝有「PCB Edge」線就照它（KiCad 庫約四分之一的 USB / RJ / DC 封裝有），沒有就依外形判斷開口、前緣齊板邊，或用 `overhang` 指定伸出量（2026-11-19）。
3. **輕量 3D 外觀：EasyEDA OBJ → VRML（`.wrl`）。** 重點在模型原點與旋轉對齊。 ✅ `lcsc_fetch` / `add_footprint lcsc=` 一併取得 EasyEDA 的 OBJ，轉成 `lcsc.3dshapes/<封裝>.wrl`。位置取封裝 3D 輪廓的中心（`c_origin` 有時不在封裝座標系）、依 `c_rotation` 旋轉、底面加 `z`；以 USB-C、SOT-23、SOT-223 的實際資料驗證，沒有參考 easyeda2kicad（2026-11-19）。

## 階段二：Layout-Aware 檢查（併入 `audit`）

1. **參考平面檢查：** 訊號跨平面分割、換層缺回流地孔（純幾何，最先做）。 ✅ `audit` 的 `plane` / `return_via`：沿每條訊號線找相鄰銅層鋪銅的網路，量跨分割、平面缺口、超出平面邊緣；換層過孔量連接兩平面的同網路回流過孔（最近、另一側、是否在 BGA 區及區內地孔數），不同平面網路則量兩網路間最近的電容。**給 agent 用：不帶門檻只回每網路總覽（最差在前、`plane_limit` 行），不下判定；門檻由 agent 以網路萬用字元對應 mm 指定**（`return_via`、`plane_gap`、`two_sided`、`bga_return_via_mm`），分級建議寫在 skill。預設每段只回一行標頭（含最差網路，約 200 token）；差分對合為一行、`pairs_only` 篩差分對；判定依規則分組（最嚴的在前，通過也有計數行）。以 cm5_minima、stickhub、pic_programmer、vme-wren、video 驗證（`client/plane_e2e.py`），另跑過 jetson、coldfire、openair、tiny_tapeout、royalblue（vme-wren 全板 1.6 s）（2026-11-19）。
2. **差分對與等長檢查：** 成對、間距一致性、skew 與長度報告；寫入 net class 差分對規則。不佈線、不繞線。 ✅ `audit` 的 `diff_pair` / `length`：長度用 KiCad 的 `LENGTH_DELAY_CALCULATION`（與網路檢視器相同：走線、過孔跨距、pad-to-die）；沿差分對每 0.1 mm 量到同層對方的間距，**依層**取目標（有自己的 net class 差分間距就用它，不採 Default 繼承的值；否則各層中位數），報 90% 耦合長度的間距範圍、未耦合長度與最長一段、過孔數。**不帶門檻只回一行標頭**（最差 skew、最多未耦合）；`length_limit` / `length_nets` 列每對與網路長度；門檻由 agent 以萬用字元對應 mm：`skew`（指出短的那條需補多少）、`uncoupled`、`pair_gap`（容差，進入自己 pad / 過孔的 breakout 不算）、`length_match`（每個 glob 一組，差分對以平均計，列出每條需補的長度）、`max_length` / `min_length`；未佈完的連線會標「length so far」。差分對規則沿用 `set_rules` 的 `diff_pair_gap` / `diff_impedance`，設定後即成為檢查目標。以 cm5_minima、stickhub、vme-wren、jetson 驗證（`client/length_e2e.py`；jetson 144 對約 2 s）（2026-11-20）。
3. **DC IR Drop：** 依銅厚、走線、過孔與鋪銅建電阻網格，找壓降熱點與過孔瓶頸；可用 `board_render` 標示。 ✅ `audit` 的 `ir_drop`：每個電源軌建電阻網路（鋪銅依格點、每格一個方塊電阻；走線一段一個電阻，T 形接點會切開；過孔與鍍通孔以 20 µm 孔壁連接各層，層距取自疊層；銅厚取自疊層），來源 pad 合為 0 V 節點、負載 pad 抽電流，用 Eigen 稀疏 LDLT 求解。格點取鋪銅的 fill（已扣異網路的 antipad），依面積自動選（預設 0.1 mm，每軌最多 60 萬格，超過就放大格點並在該列註明，粗於 0.15 mm 時提示；`ir_mesh_mm` 可指定更細，上限 200 萬格；0.1 mm 與 0.05 mm 相差 1–2%，粗格點偏高、偏保守）。報最差負載的壓降與位置、未填充的鋪銅、損耗分佈（走線 / 鋪銅 / 過孔）、最忙的過孔、負載最重的走線（有電流時附 IPC-2221 所需線寬）。來源自動猜（輸出腳、電感、連接器、模組）並註明，`ir_source` / `ir_loads` 覆寫；預設負載為該軌上的 IC，電流依 pad 數分配。**不帶門檻只回一行標頭**；沒給 `currents` 時壓降以每 A 計且門檻不判定；門檻由 agent 指定：`ir_drop`（mV）、`via_current`（A）。`board_render` 新增 `marks` 標示熱點（多點時取聯集加邊距）。找不到來源的電源軌依總銅面積（鋪銅 fill + 走線長×寬）排序列出，附 pad 數與面積，少 pad 的大電流輸入軌也會排在前面。手算驗證（cm5 +1V8：0.96 mm、0.15 mm 走線 3.14 mΩ，求得 3.15）。以 cm5_minima、vme-wren、jetson 驗證（`client/irdrop_e2e.py`）（2026-11-20）。
4. **平行走線串擾初篩：** 長距離、間距小於 3H 的耦合線對。 ✅ `audit` 的 `crosstalk`：同層不同訊號網路的走線平行（10° 內）且邊到邊間距小於 3H（H 為到最近參考平面的介電厚度，與阻抗工具相同），以線段解析計算耦合長度（直線段上間距線性變化，區段精確；cm5 HOTPLUG/CEC 9.9 mm，以 0.01 mm 取樣驗證得 9.92），依網路對加總、分層列出、報最近間距（mm 與 H 倍數）與位置。差分對兩成員合為一個名稱、彼此不算，對其他網路取耦合較多的成員（不重複計算）；電源與地不列。**不帶門檻只回一行標頭**（最長的一對），`xtalk_limit` / `xtalk_nets` 列每對；門檻由 agent 指定：`xtalk_length`（mm，一對取兩網路中最嚴的 glob）、`xtalk_spacing`（H 的倍數）。2 層板 H 為整片板厚，3H 會涵蓋多數鄰線，標頭會註明。只看同層（相鄰訊號層的寬邊耦合未篩）。以 cm5_minima、stickhub、vme-wren、jetson 驗證（`client/xtalk_e2e.py`；jetson 全 audit 約 2.5 s）（2026-11-20）。
   - ✅ **參考平面依鋪銅判定（impedance 與 crosstalk 共用）：** 原本取緊鄰銅層當參考平面，遇到相鄰兩層訊號層（L2 訊號、L3 訊號、L4 GND）時 H 偏小，串擾會漏篩、阻抗會算低（不保守）。改為共用 `impedance::Planes()`：該層鋪銅（fill，未填充時取外框）佔板框 40% 以上才算平面；尚無鋪銅的層，Board Setup 標 power/mixed 或整層沒有走線（待鋪的平面）也算，不依層名或層類型（openair-max 的 In1 標 power 但只鋪 25%、有 194 段走線，判為訊號層）。往上、往下各找最近的平面，中間跨過的訊號層銅厚計入 H，輸出標「across In2.Cu」；內層只有一側有平面時當 embedded microstrip（另一側整片板當覆蓋層）。`impedance` 的 note 列出每層覆蓋率；`crosstalk` 標頭列出相鄰、中間沒有平面的訊號層（寬邊耦合，未篩）。以 vme-wren 拿掉 In1 鋪銅驗證：F.Cu 改參考 In3，H 0.119 → 0.477 mm（含 In1、In2 銅厚），耦合線對 598 → 1228（`client/impedance_e2e.py`、`client/xtalk_e2e.py`）（2026-11-20）。
   - **技術債（初篩階段先不做）：** 兩線之間夾接地護線仍算耦合（偏保守）；耦合長度等權加總，沒有依間距衰減（約 1/(1+(S/H)²)），目前靠最近間距（H 倍數）讓 agent 搭配判讀。~~`plane` 檢查仍只看緊鄰層的鋪銅，跨過訊號層的參考平面尚未納入~~ ✅ `plane` / `return_via` 改用 `impedance::Line` 的參考平面（依鋪銅、跨過中間訊號層，與 `impedance`、`audit impedance` 相同；整板沒有平面時仍取緊鄰銅層），標頭列出跨層的層；中間訊號層的局部鋪銅壓在線下時報「over local copper」（長度、最長一段、哪個網路哪一層，0.5 mm 以下不列），只提示不判定，缺口處若有局部銅也註明。vme-wren 拿掉 In1 鋪銅後在 In1 鋪 10 mm GND：IO30 報 10 mm over GND In1；原板 19 對 / 條走在 In2 的局部電源銅上（SFP0.RX 9.8 mm 在 VCC_PSPLL 上）（2026-11-21）。~~`impedance` 剖面排除各網路自己 pad 周圍 0.5 mm 時從 pad 中心畫圓~~ ✅ 改為從 pad 銅皮邊緣（實際形狀）往外 0.5 mm（beta.5，2026-11-21）。覆蓋率是同層所有鋪銅加總，幾塊互不相連的電源島合計過 40% 也會被當平面；`plane` 本來就抓跨分割，風險低。
5. **沿線阻抗剖面：** 依實際線寬與疊層計算，與目標阻抗比對。 ✅ `audit` 的 `impedance`：每條訊號線逐段計算（單線每 1 mm、差分對每 0.25 mm，另在其他層鋪銅邊緣處切開），每段依實際線寬、所在層與**該點上下實際的銅**決定截面：參考平面取自 `impedance::Planes()`，中間訊號層若有局部鋪銅壓在線下就以它為該處的平面，平面有缺口時取更遠一層的銅，上下都沒有銅則為 off plane（不算 Z，交給 `plane` 檢查）；差分對量到對方的實際間距算 Zdiff（超過 4H 且 0.5 mm 以上視為未耦合，取 2×Z0）。算式與 `impedance` 工具相同（vme-wren PCIe REFCLK：B.Cu 120.7、In9.Cu 95.2 Ω，兩邊一致）。各網路自己的 pad（從銅皮邊緣）與過孔周圍 0.5 mm 不計（breakout 處的不連續由 pad 本身主導）。**不帶目標只回一行標頭**：差分對在前，各類主 Z 範圍、多少網路偏離自身主 Z 10% 以上達 1 mm、偏最多的一條與原因（換層、線寬、局部鋪銅、平面缺口）及位置；`z_limit` / `z_nets` 列每條；目標由 agent 指定：`z_target`（Z0，單線）、`z_diff_target`（Zdiff，差分對）、`z_tolerance`（%，預設 10）、`z_off_mm`（允許偏離長度，預設 1 mm）。以 vme-wren 拿掉 In1 鋪銅、再在 In1 鋪一塊 10 mm 的 GND 於 F.Cu 線下驗證：該段 61.6 Ω（等於原板 F.Cu 參考 In1 的值），其餘參考 In3 為 108.3 Ω（`client/zprofile_e2e.py`；cm5、vme-wren、jetson；jetson 全 audit 約 3.4 s）。過孔、pad 與同層共面地（CPWG）未納入（2026-11-20）。

## 階段三：區域模擬與二階模型

**開工條件：** 先統計 `jlcparts` 中 MOSFET、LDO、Op-Amp、電感、二極體各參數的實際填寫率，用數據決定哪些類別可行。 ✅ `ci/param_coverage.py`，結果在 [docs/PARAM_COVERAGE.md](docs/PARAM_COVERAGE.md)（2026-11-11 的 jlcparts 快照，有庫存的零件；「可用」＝有填且能讀成該單位的數字）（2026-11-21）：
   - **可行：** MOSFET（Vds、Id、Rds(on)、Vgs(th) ≥99%，Ciss 96%、Qg 94%、Crss 93%、Coss 86%）；Op-Amp（Vos、Ib、CMRR ≥97%，Slew 95%、GBW 92%、en 88%）；LDO（Vin、Vout、Iout ≥98%，Iq 89%、Dropout 88%、PSRR 76%）；電感 L、DCR、額定電流 ≥93%；二極體 Vr、If、Vf ≥99%，開關二極體 trr 96%；Zener Vz、Zzt ≥96%。
   - **缺：** 電感 Isat 58%（basic / preferred 0%）、SRF 23%；快恢復二極體 trr 61%；**Schottky 的 Cj、所有二極體的 Cj、MLCC 的 ESR 與 DC Bias 全為 0%**；LDO 的 PSRR 只有一個數字，沒有頻率；Rds(on)、Qg 只有一個測試條件（`12mΩ@10V`）。
   - **影響：** 第 1 項先做上面「可行」的參數；缺的不猜數字，標成 unknown。MLCC DC Bias / ESR 沒有資料，第 2 項的 Corner Sweep 只能做容差（`Tolerance` 84%），DC Bias 要另外決定（依介質與尺寸的通用曲線，或不做）；PDN Z(f)（第 3 項）需要 ESR / ESL，同樣受限。

1. **二階電氣屬性抽取：** MOSFET（Rds(on)、Qg、Ciss/Coss/Crss、Vgs(th)）、電感（DCR、SRF）、二極體（Vf、trr、Cj）、Op-Amp / LDO（GBW、SR、Dropout、PSRR）。 ✅ 零件索引新增 `params` 表（`ci/part_params.py` 讀屬性，SI 單位，附資料表的測試條件，如 Rds(on) `@10V`、Vf `@1A`；8 類 38 個參數，76k 顆、42 萬列，壓縮後多 6 MB）與 `param_names`。`lcsc_search` 每筆列出參數，`where {"rds_on": "<=20m", "vds": ">=30", "vgs_th": "1..2.5"}` 篩選（門檻由 agent 給；缺該參數的零件不列入）；舊索引會說明要 `update=true`。Cj 與 MLCC 的 ESR / DC Bias 沒有資料，不做。測試 `client/search_e2e.py`（2026-11-21）。
2. **區域子電路 headless 模擬（`ngspice` 外部執行檔）：** 依零件、圖紙或 N-hop 圈選子電路，自動注入邊界電源與激勵；封裝寄生查表與 MLCC DC Bias / 容差 Corner Sweep；回傳結構化量測（紋波、頻寬、過衝、穩態誤差）與輕量波形圖。 ✅ 工具 `simulate`（2026-11-21）：
   - **圈選：** `refs`、`sheet`，或 `around` + `hops`。不穿過地；穿過電源時只帶兩腳零件，所以 LDO 會帶到自己的電容，不會帶到整條電源軌上的 IC。
   - **模型：** 依零件索引的參數產生 R、C、L、二極體 / Zener、MOSFET（level 1）、LDO 與 Op-Amp（行為模型）；有 `Sim.*` 的零件用 KiCad 自己的模型與 `.include`。
   - **MLCC DC Bias（決定：做估算、一律標示、可覆寫）：** 由容值 ÷ 體積推介電層厚度，再依電場 V/d 計算 `1/(1+(E/2.5 V/µm)^1.5)`（Y5V / Z5U 用 1 V/µm，C0G 不衰減）。先跑一次 DC 求每顆電容的實際偏壓再代入。0402 10 µF X5R 在 3.3 V 剩 22%，原廠曲線約 20–30%。報表寫成 `C_eff ≈ 2.24 µF (est. DC bias: 22%, layer ~0.58 µm, 5.7 V/µm)`；`dc_bias false` 關閉。
   - **ESR / ESL 的典型值（為第 3 項鋪路）：** MLCC 的 ESR 為 15 mΩ·(C/1 µF)^-½，下限 2 mΩ；ESL 依封裝（0402 0.4 nH…）；鋁電解、鉭質、高分子各有典型值。全部標「~ / est.」。
   - **逐顆覆寫：** `models {ref: {...}}` 可覆寫任一參數，或掛外部 `.lib` 的 subckt。
   - **電源與負載：** 電源由 agent 給（DC / pulse / sin / ac）；沒給的電源依網路名稱猜電壓，並註明。負載可以是電流、脈衝或電阻。
   - **分析：** `op`、`tran`、`ac`，`corners` 用公差上下限再跑兩次（公差取自資料，沒有就假設並註明）。
   - **量測，不下判定：** 平均、漣波、min / max、階躍的過衝 / 上升 / 穩定時間、−3 dB 點、電源電流與功率、電感電流（附 Isat / Irated）、LDO 輸出電流（附 Iout），另附一行字元波形。
   - **沒有模型的零件：** 列出並在該處斷開。
   - **ngspice：** Windows 壓縮檔附官方 ngspice-42（BSD）。
   - **測試：** `client/sim_e2e.py`，與手算比對。RC 的 −3 dB 為 1/(2πRC)，上升時間 2.2RC；隨耦器 −3 dB 在 GBW；LDO 輸入電流等於負載加 Iq；另跑 KiCad 的 rectifier 範例。
   - **適用邊界（報表與說明都有寫）：** `corners` 是全上限／全下限，不是最壞組合或蒙地卡羅（分壓、同比例 RC 會互相抵銷）；DC Bias 取自先跑的 op（tran 取量測窗平均），適用線性電路，切換式電源與 AC 耦合節點不準（需要時再做從 tran 穩態回推）；穿過電源軌時會帶進所有兩腳零件（整條軌的去耦電容，對 LDO 穩定度是對的），像 LED 限流電阻這類另一端沒帶進來的，會在「cut at the region's edge」列出該處的零件與外面的零件，可加 hops 或 `exclude`。
   - **未做：** PCB 寄生（安裝電感，第 3 項）、飽和與限流、S 參數、波形圖片（以字元波形代替）。
3. **局部 PDN 交流阻抗 Z(f)：** 電容二階模型加 PCB 實際迴路電感，找反諧振尖峰（需要上兩項）。
4. **傳輸線反射與串聯端接建議。**

## 階段四：協同介面

1. **桌面 Co-pilot 面板：** 即時活動紀錄、「暫停 Agent 寫入」開關。
2. **遠端 Web 2D 視圖：** 電路圖檢視、網路高亮、協同狀態提示，輪詢改為主動推送。

## 階段五：製造與生態延伸

1. **組裝圖匯出、多實例重複圖紙**（同一子圖紙多次實例化）。
2. **MCP Resources / Prompts**，降低初始化上下文成本。
