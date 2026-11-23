# 交接：beta.13 的工作清單（2026-11-24）

> **狀態（2026-11-24）：** v0.1.0-beta.12 已打 tag 並發布（pre-release）。階段 1 完成，在本機 commit，等使用者確認後開始階段 2。

beta.12 已發布（NEXT_beta10 第一優先 1–10 項、盲測 S1 的修正）。這份清單合併三個來源：
- 盲測 S2（四層 RP2040 板）的回報；
- `docs/NEXT_beta12.md` 裡還沒做的項目；
- MCP 工具調用次數的統計。

方向不變：只做對任何板子都有用的改進。

## 已定案的 beta.13 計畫（2026-11-24，使用者確認）

### 原則
避免對 StickHub 過度擬合。優先做通用修正，在多種板子上驗證一般化。

### 階段 1（先做）：修正 S2 回報的問題
- **順序：** 從〈第一優先〉第 1 項開始：同一組 0.324 / 0.15 mm 的線對，`impedance` / `set_rules` 算出 90 Ω，`audit` 量到 108 Ω。先找出哪一邊錯，再統一兩邊的平面模型。
- **接著：** 第 2–6 項，以及第 8 項（NEXT_beta12 留下的項目）。第 7 項（back_decaps 把按鈕放到底面）也一併做。
- **完成條件：**
  - 每項都有 e2e 測試。
  - `python3 -I client/run_all.py` 全部通過（目前 40 個）。
  - 先在本機 commit。

### 階段 2：合併工具
- **範圍：** 只做下方〈收斂合併建議〉的 A 類：
  - undo / redo / sch_undo / sch_redo；
  - get_rules → set_rules；
  - check_drc → run_drc；
  - flush_ratsnest → board_summary；
  - open_board + new_project → project；
  - 三個 export 合成一個。
- **board_render_3d：** 若很容易，也併入 `board_render {view: "3d"}`。
- **舊名稱：** 保留一版作為別名，回應時提示新名稱。
- **一起更新：** SKILL.md、TOOLS.md、MCP Resources / Prompts、e2e 測試。
- **不動：** simulate、pdn、reflection、board_diff、teardrops。launch_kicad / close_kicad 也維持獨立。

### 階段 3：審查型盲測（階段 1、2 都通過之後）
- **作法：** 在 CM5 或 Jetson 板上跑一輪審查型盲測，例如「審查這塊板並提出問題」。
- **目的：** 公平評估 audit、simulate、pdn、reflection、board_diff、export_assembly 等分析工具。之後再決定它們要保留還是合併。

### 發布流程
1. push main。
2. 等 Windows CI 變綠。
3. 推 annotated tag `v0.1.0-beta.13`。

使用者已同意 CI 綠燈後直接推 tag。不過自動模式的權限檢查可能會擋下推 tag：被擋時，請使用者自己推。

## 階段 1 的結果（2026-11-24，本機 commit，尚未推送）

- **第 1 項（90 vs 108 Ω）：** 兩邊的平面模型本來就相同（`impedance::Line` / `LineOver`，都是「microstrip over In1.Cu, 0.2104 mm」）。差別在 S2 的走線：USB_D_P 與 USB_D_N 在 R3 與 U4 之間相距 1.3–1.9 mm，沒有耦合，`audit` 正確地取 2 × Z0（2 × 54 ≈ 108 Ω），但訊息沒有說「uncoupled」，看起來像平面不同。
  - 修正：`audit impedance` 對沒耦合的線段寫「uncoupled: its partner 1.32 mm away (over 0.84 mm: 2 x Z0)」，每組差分對列出沒耦合的長度，z_diff_target 的警告說明原因；標頭列出每層的參考平面（與 impedance / set_rules 同一套）。
  - 測試：`impedance_e2e` [pair in audit]：同一組 0.324 / 0.15 mm 線對，`audit` 量到的 Zdiff 等於 `impedance` 算的；分開 2 mm 的線對標為 uncoupled。
- **第 2 項：** `route to=zone` 的最後一個 via waypoint 在鋪銅上時，只打那一顆過孔（S2 是兩顆、繞 2 倍路）；起點就在 via 上時只放過孔；`query_area via_net` 列出 QFN 焊盤環內的位置（標 `*`）；`stitch_vias` 可處理只鋪一層的網路（要給 area）。
- **第 3 項：** `auto_place` 在細腳距 IC（16 支以上 SMD 腳、腳距 ≤ 0.65 mm）焊盤外留 2 腳距 + 1 線寬的逃線環（`escape_room`，0 關閉）。
- **第 4 項：** `route` / `route_short` / `fanout` 從細腳距焊盤出發（或進入）時，class 線寬放不下就先縮到放得下的最大寬度再加寬（「necked to 0.30 mm out of U1.10」）；間隙剛好等於 clearance 會接受（已有測試）。
- **第 5 項：** EasyEDA 標成 bidirectional 的電源腳依名稱改成 power_in（VREG_VOUT 等輸出為 power_out），回應列出改了哪些腳。
- **第 6 項：** `query_area` 的 custom 形狀焊盤用 bbox（A4B9 0.6x1.3）。
- **第 7 項：** `back_decaps` 只翻電容（C*）。
- **第 8 項：** edge 規則 `along: true` 與每個邊緣零件的方向；`sch_connect partial=true`（對半重試，只列出有問題的腳）；`lcsc_fetch pin_types={...}`；沒有編號的外殼焊盤（`unnumbered_pads`）；`run_drc` 的 starved thermal 建議；`board_render` 畫出 NPTH 孔的外框（原因：孔用背景色畫，沒有鋪銅時看不見）；`hole_inset` 量到孔中心，寫進說明，太小會拒絕。
- **新測試：** `escape_e2e`（第 2、3、4、7 項與 starved thermal），run_all 變成 41 個。
- **新發現（尚未處理）：** `set_rules` 先依名稱 pattern 指定 class、之後才用 `add_footprint` 加入的零件，從它的焊盤起線時寬度是板子最小值（0.09 mm，「board setup constraints」），不是 class 的寬度；同一個網路從另一個零件起線卻正確。sync_pcb 的流程（S2）沒有這個問題。先用 `add_footprint` 再 `set_rules` 就正常。

## 盲測 S2 的結果

- **規格：** `run/exp/s2-spec.md`。RP2040、四層，內層是完整的 GND 平面和 3V3 平面，另有 USB 90 Ω 差分對、QSPI flash 和晶振。
- **專案：** `run/exp/s2`。
- **結果：** ERC 0 錯 / 13 警告，DRC 0 錯 / 1 警告，未佈線 0，出廠檔齊全。
  - 平面檢查：內層平面沒有被訊號切開。
  - USB 線對：
    - 接頭端的 USB_D 有 1.33 mm skew。原因是 USB-C 的 A/B 兩排腳在接頭底下交叉，只能經過孔換層。
    - audit 量到 108 Ω，不是預定的 90 Ω。
- **最花時間的兩件事：**
  - QFN（0.4 mm 腳距）電源腳的逃線。
  - `auto_place` 把電容放進 IC 的逃線通道後，要手動重擺。

## 第一優先：通用改進

1. **`impedance` / `set_rules` 和 `audit impedance` 的結果不一致。**
   - 同樣 0.324 / 0.15 mm 的線對：前者算出 90 Ω，後者量到 108 Ω。
   - 兩邊應該共用同一個平面模型（`impedance::Planes()`）。要查出差在哪一層：In1 GND 平面的參考距離，或線對旁邊的鋪銅。
   - 修好後，回應要說明採用的參考平面。
2. **把過孔打進平面。**
   - `route to=zone` 給了 via / 換層 waypoint 時，在那一點只打一顆貫孔，接上該層的平面。現在會多打一顆並繞路。
   - 允許過孔放在 QFN 的焊盤環內。
   - `stitch_vias` 要能處理只鋪一層的網路（In2.Cu 的 +3V3）。
3. **`auto_place` 預留細腳距 IC 的逃線區。**
   - QFN、QFP、BGA 周圍預設留一圈 keepout，大約一到兩個腳距加一條線寬，讓去耦電容和它們的過孔不擋住逃線。
   - 這是第 9 項（track_room）的延伸：那一項處理的是既有走線，這一項處理還沒佈的逃線。
4. **線寬配合腳距。**
   - `route` / `fanout` 從細腳距焊盤出發時，自動縮到 min(class 線寬, 焊盤間隙 − 2 × 間距) 再加寬。
   - 間隙剛好等於 clearance 時要接受。S2 遇到的情況：0.2 mm 線在 0.4 mm 腳距上被拒，相鄰的腳卻可以。
5. **腳型別推定擴充。**
   - EasyEDA 的資料把 RP2040 的 IOVDD、DVDD、VREG_* 標成 bidirectional，造成幾個後果：
     - `audit` 的去耦檢查跳過主 IC。
     - `ir_drop` 找不到 +1V1 的電源。
     - ERC 出現 13 個警告。
   - 名稱明確是電源的腳（IOVDD、DVDD、AVDD、VREG_*、USB_VDD、ADC_AVDD、VCC*、VDD*）即使資料標了 bidirectional，也改成 power_in。回應要列出改了哪些腳。
6. **`query_area` 的 custom 形狀焊盤顯示 0x0。** 例如 USB-C 的 A4B9 / A1B12。改用 bbox。
7. **`auto_place back_decaps=true` 把按鈕 SW2 放到底面。** 只有去耦電容才可以翻到底面。
8. **NEXT_beta12 留下的項目：**
   - 邊緣排針「沿邊」的方向。
   - `sch_connect partial=true`：S1、S2 都因為一個衝突，整批 30–70 支腳失敗。
   - 修改腳型別的工具。
   - USB-C 外殼焊盤沒有編號。
   - starved thermal 的建議。
   - `board_render` 有時沒畫出 NPTH 孔。
   - 定義 `hole_inset`。

## 文件（SKILL.md）

- 細腳距 QFN 電源腳的逃線作法：焊盤環內打過孔、先拉短線再打孔、小過孔（JLC 0.45 / 0.2 mm）。
- 本地標籤在板上的網路名稱是 `/NAME`。`set_rules` 的樣式回報已經會提示這一點；SKILL.md 也要寫。
- USB-C 的 A/B 兩排腳交叉：pair order 不報錯，但佈線仍需要一對過孔。
- `to=zone` 搭配 via / waypoint 的行為。

## MCP 工具調用統計（2026-11-24）

**資料：**
- 來源：16 份 transcript，包括盲測第一到五輪、S1、S2，以及開發時的代理工作階段。
- 調用次數：直接調用 1968 次，`batch` 內又有 1589 次。
- 工具總數 68 個，其中 16 個從未被調用。

**偏差：** 這些幾乎都是「從零設計一塊板」的任務。分析和審查類工具沒被用到，是因為任務沒要求，不代表它們沒用。決定之前，應該先跑一輪「審查既有設計」的盲測，例如 cm5 或 Jetson 的 review：`audit`、`simulate`、`pdn`、`reflection`、`board_diff`、`export_assembly`。

**最常用：**

| 工具 | 次數 |
|---|---|
| route | 617 |
| sch_add_part | 551 |
| move_footprints | 310 |
| batch | 284 |
| lcsc_search | 208 |
| sch_move | 180 |
| query_area | 177 |
| board_render | 99 |
| sch_connect | 91 |
| route_short | 85 |

錯誤率最高的是 `route`：55 次錯誤，約 9%。

**0 次：** add_footprint、delete_footprints、pcb_set_fields、teardrops、redo、sch_redo、board_render_3d、export_assembly、board_diff、lib_register、sch_export_pdf、simulate、pdn、reflection、launch_kicad、close_kicad。

**1–6 次：** open_board 1、sch_annotate 2、get_rules 3、board_outline 5、check_drc 5、export_fab 5、flush_ratsnest 6、sch_summary 6。

### 收斂合併建議（68 → 約 59）

**A. 明確可以合併：** 這些工具都很薄、同一領域，而且很少用到。

| 合併 | 現在 → 之後 | 說明 |
|---|---|---|
| `undo` / `redo` / `sch_undo` / `sch_redo` | 4 → 1 | `undo {target: "board" \| "schematic", redo: true}`。redo 兩個都是 0 次 |
| `get_rules` 併入 `set_rules` | 2 → 1 | `set_rules` 不帶參數就是查詢（它本來就會回傳完整規則） |
| `check_drc` 併入 `run_drc` | 2 → 1 | `run_drc {refs, local: true}`：只檢查零件周圍，不移動 |
| `flush_ratsnest` 併入 `board_summary` | 2 → 1 | `board_summary` 直接給完整的未連線數。各工具的計數已經統一，會標出延後的網路 |
| `open_board` + `new_project` | 2 → 1 | `project {open: path}` / `project {new: {...}}` |
| `export_fab` / `export_assembly` / `sch_export_pdf` | 3 → 1 | `export {fab: {...}, assembly: {...}, schematic_pdf: {...}}`，可以一次出多份 |

**B. 可以考慮：** 參數差異較大，合併後 schema 會變長，要權衡。
- `board_render_3d` 併入 `board_render {view: "3d", ...}`（1 個）。
- `add_footprint` / `delete_footprints` / `pcb_set_fields` 合成 `footprints {add | delete | set}`（−2）。這三個是給不走電路圖的板上直接編輯和桌面協作用的，從零設計的流程不會用到。
- `lib_register` 併入 `lib_create_*`（−1），三者參數不同，效益小。

**C. 保留：**
- `simulate`、`pdn`、`reflection`、`teardrops`、`board_diff`：審查和分析領域，要用審查型盲測驗證後再決定。
- `launch_kicad` / `close_kicad`：有 `--allow-launch` 的安全邊界，宜保持獨立。

**注意：** 合併會改變工具名稱，要一起處理：
- 舊名稱保留一版作為別名，回應時提示新名稱。
- 更新 SKILL.md、TOOLS.md、MCP Resources / Prompts。
- 更新所有 e2e 測試。
