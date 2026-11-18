# KiCadD 路線圖（2026-11-18 定稿）

起點：`0.1.0-alpha.13`。Linux 上 24 套 e2e 全部通過（`client/run_all.py`）；Windows CI 跑 smoke、fab e2e、SMD e2e。

## 原則

1. **維持精簡架構。** 不加回 OpenCASCADE，不恢復 wxWidgets 模擬視窗（保留 SPICE 網表匯出）。
2. **不建靜態模型庫、不寫爬蟲。** 電氣參數只從現有的 `jlcparts` 索引抽取。
3. **不做自動佈線、不做自動等長繞線（meander）。** Headless 的產出是擺件；推擠、差分對、等長由人在桌面版用 KiCad 原生工具完成。Agent 端只做**檢查**。
4. **先做確定性的 Layout-Aware 檢查，再做模擬。** 前者只用已有的板子幾何、疊層與銅厚，不需新相依套件；模擬依賴模型品質，開工前先量參數覆蓋率。
5. **新增的模擬後端以外部執行檔呼叫**（`ngspice`），不連結進精簡 build。

## 階段零：Beta（功能凍結，補強可靠度）

1. **電路圖 PDF 匯出**（MCP 工具，沿用上游的 PDF plotter）。
2. **自動重新編號 Annotate**（MCP 工具，沿用上游的 annotate；含桌面版共用 undo）。
3. **Windows 測試補齊。** headless 的 `tools`、`setup`、`history` 等加進 Windows CI；桌面版 e2e（`desktop`、`desktop_sch`、`desktop_autoplace`）在 Windows 上至少手動跑過一次，能自動化就自動化。
4. **open_board 的「未存檔」誤報。** undo 回原狀後仍拒絕：先在拒絕訊息附上 board_diff 結果（內容與檔案相同時說明 `discard=true` 不會丟東西）；真正清除修改旗標列入後續 TODO。 ✅ 拒絕訊息已附 board_diff（2026-11-18）。
5. **技術債：** 刪除封裝後殘留的空網路等小問題。 ✅ 刪除封裝（及 undo 新增、redo 刪除）時，沒有其他東西在上面的網路一併移除，undo 會放回（2026-11-18）。
6. **Beta 說明文件：** 安裝、skill 用法、10.99 檔案格式限制、「做得到 / 做不到」一覽。 ✅ [docs/GUIDE.md](docs/GUIDE.md)（2026-11-18）。

## 階段一：交付閉環（beta.x）

依成本由低到高：

1. **控制阻抗線寬計算。** 重用 pcb_calculator 的 transline 計算與內建 26 種 JLCPCB 疊層，估算單端 / 差分線寬並寫入規則。
2. **`auto_place` 連接器伸出板邊。** USB-C、RJ45、DC Jack 等依規則突出板框。
3. **輕量 3D 外觀：EasyEDA OBJ → VRML（`.wrl`）。** 重點在模型原點與旋轉對齊；參考 easyeda2kicad 的偏移處理前先確認授權。

## 階段二：Layout-Aware 檢查（併入 `audit`）

1. **參考平面檢查：** 訊號跨平面分割、換層缺回流地孔（純幾何，最先做）。
2. **差分對與等長檢查：** 成對、間距一致性、skew 與長度報告；寫入 net class 差分對規則。不佈線、不繞線。
3. **DC IR Drop：** 依銅厚、走線、過孔與鋪銅建電阻網格，找壓降熱點與過孔瓶頸；可用 `board_render` 標示。
4. **平行走線串擾初篩：** 長距離、間距小於 3H 的耦合線對。
5. **沿線阻抗剖面：** 依實際線寬與疊層計算，與目標阻抗比對。

## 階段三：區域模擬與二階模型

**開工條件：** 先統計 `jlcparts` 中 MOSFET、LDO、Op-Amp、電感、二極體各參數的實際填寫率，用數據決定哪些類別可行。

1. **二階電氣屬性抽取：** MOSFET（Rds(on)、Qg、Ciss/Coss/Crss、Vgs(th)）、電感（DCR、SRF）、二極體（Vf、trr、Cj）、Op-Amp / LDO（GBW、SR、Dropout、PSRR）。
2. **區域子電路 headless 模擬（`ngspice` 外部執行檔）：** 依零件、圖紙或 N-hop 圈選子電路，自動注入邊界電源與激勵；封裝寄生查表與 MLCC DC Bias / 容差 Corner Sweep；回傳結構化量測（紋波、頻寬、過衝、穩態誤差）與輕量波形圖。
3. **局部 PDN 交流阻抗 Z(f)：** 電容二階模型加 PCB 實際迴路電感，找反諧振尖峰（需要上兩項）。
4. **傳輸線反射與串聯端接建議。**

## 階段四：協同介面

1. **桌面 Co-pilot 面板：** 即時活動紀錄、「暫停 Agent 寫入」開關。
2. **遠端 Web 2D 視圖：** 電路圖檢視、網路高亮、協同狀態提示，輪詢改為主動推送。

## 階段五：製造與生態延伸

1. **組裝圖匯出、多實例重複圖紙**（同一子圖紙多次實例化）。
2. **MCP Resources / Prompts**，降低初始化上下文成本。
