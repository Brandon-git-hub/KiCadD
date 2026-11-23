# 交接：beta.10 之後的工作清單（2026-11-23）

beta.10 已修正第四輪留下的 4–8 項、非直線的 via 搜尋，以及第五輪發現的路由器當機、`route_short` 逾時與 `route` 的兩個問題（見 `ci/release-notes.md`）。本文件是之後的清單。

> **狀態（2026-11-23）：** 〈第一優先：通用改進〉的 1–10 項已完成，排進 beta.12（見 `ci/release-notes.md`）。第 9 項的「每個通道的可佈性估計」只做到焊盤層級：擺完後列出最擠的焊盤，以及旁邊放不下一條線的焊盤數。

## 方向

- **StickHub 改當壓力基準。** 16.5 mm 寬、雙面、QFP 加 8 對差分線的 7 埠 hub 不再追求未佈線 0，也不為它在 SKILL.md 加專用作法，例如「密集兩層 QFP 佈線食譜」。之後仍可以用同一份規格跑，觀察工具在極端密度下會不會出錯（當機、逾時、誤導的回應）。
- **測一般化。** beta.10 之後改用其他類型的板子做盲測：
  - 一般密度的兩層板，例如感測器板或小型電源板。
  - 四層 MCU 板，有完整的 GND / 電源平面。
  - 比較的重點是工具通不通用，不是某一塊板的分數。
- **排序原則：** 對任何板子都有用的改進排前面，只對 StickHub 有用的不做。

## 第一優先：通用改進

1. **找空的過孔位置，並提供焊盤尺寸。**
   - **現況：** `query_area` / `query_net` 只給焊盤中心。代理只能猜過孔位置，猜錯一次就是一次失敗的 route 加一次查詢。第五輪每個階段都提到這點。
   - **建議：**
     - `query_area` 的焊盤列加上尺寸，例如 `PAD U1.5 [VDD] F.Cu @x,y 0.3x1.5`。
     - 新工具或新參數：在 (x, y) 半徑 r 內，列出某網路可放 through via 的空位（兩層都沒撞、不在 IC 本體下），近的先列。
     - 實作可沿用 `SESSION::ViaFits`（beta.10 的格點搜尋已在用）。
2. **各工具的未連線數一致。**
   - **現況：** `route` / `move_footprints` / `undo` 回 `excl. deferred nets: GND`；`route_short`、`fanout`、`audit`、`stitch_vias` 則把 GND 算進去，或各算各的。代理看到 87 → 111 這種跳動，會以為壞了東西。
   - **建議：** 同一個函式統一算，每個數字都說是否含延後的大網路，例如 `unconnected=111 (GND 24 included)`；或者全部都含，代價是一次 flush。
3. **`set_rules` 的樣式比對提示。**
   - **現況：** 階層圖紙裡的網路名稱有 `/` 前綴（`/USBUP_P`），`USB*_P` 默默只配到 16 個裡的 14 個。
   - **建議：**
     - 每個 assign 樣式回報配到幾個網路。
     - 沒配到，或有名稱只差一個 `/` 前綴的網路沒配到時，警告並給 `*USB*_P` 這類寫法。
4. **電源腳的型別推定。**
   - **現況：** FE2.1 的 VD33、VD18、VD_PLL 被 `lcsc_fetch` 判成 passive，`audit` 的去耦檢查因此只看到 2 支電源腳，主 IC 的 1.8 V / 3.3 V 軌沒有檢查。這和 beta.9 清單裡 USB2517 的 VDD18 / VDD18PLL 是同一個問題。
   - **建議：**
     - `VD`、`VDD`、`VCC`、`AVDD`、`DVDD` 加數字或電壓字樣的腳名，判為 power_in。
     - IC 內部穩壓的輸出腳（腳名有 18 / 1V8 / 33 且同名的腳只有一支）也判為 power_in；ERC 若因此要求驅動源，就說明用 PWR_FLAG。
5. **LCSC 資料來源一致。**
   - **現況：** `lcsc_search` 說「JLC stock 9414 / extended」，`sch_add_part` / `lcsc_fetch` 卻說「no JLC assembly」（C49451919、C29780639）。
   - **建議：** 兩邊都標出資料來源與日期（索引 `jlcparts 2026-11-xx` 或 live）；不一致時，在 fetch 的回應直接說「the index said ... on <date>」。之前回報的庫存顯示不符（C96123、C15008）也是同一項。
6. **`sch_connect` 的錯誤回報。**
   - **現況：** 一次大呼叫失敗時，回應是一串「asked +5V, on +1V8」，看不出真正的原因（C2 的標籤沒有空位）。
   - **建議：**
     - 先報根本原因（第一個撞到的點、哪支腳、哪個網路），其餘的連帶結果收成一行。
     - 根本修正（beta.9 清單已有）：選位時一併避開同一次呼叫裡剛放的 stub / 標籤。
7. **顯示 no-connect。** `sch_part` 把 NC 腳顯示成「(unconnected)」，`sch_netlist` 也沒有 NC；ERC 卻知道。兩者都應標 `NC`。
8. **`lcsc_search` 的數值。** `value: "12MHz"` 不被接受。晶振、振盪器要接受頻率；電感、保險絲的電流要接受 A；封裝名稱裡的字也要能比對（beta.9 清單已有）。
9. **`auto_place` 與既有走線。**
   - **現況：** 板上已有走線時，`auto_place` 會把零件放進比一顆過孔還窄的通道（第五輪：1.1 mm 的通道、去耦電容），之後 `route_short` 全部「none fits」。
   - **建議：**
     - 通道寬度小於「零件寬 + 兩側走線所需」時不放。
     - 讓 agent 指定保留區（每面的禁區 / 走線通道，beta.9 清單已有）。
     - 擺完後回報可佈性估計（每個通道的寬度與要通過的線數）。
10. **pair order 檢查的限制要講清楚。**
    - **現況：** 佈線後，有 U 形逃線（從腳排出去、往下、再從內側進焊盤）的差分對一直被列為反序，代理分不出哪些是真的。
    - **建議：** 佈線後改沿實際路徑比對 P / N 的相對位置；或者至少在已有佈線時註明「U 形逃線一定顯示為反序，看 `audit diff_pair` 的耦合長度」。

## 不排入（與方向衝突）

- **差分對成對佈線（P / N 一起、照 class gap）。** 第五輪的第二建議。依既定方向，差分對與等長只做檢查，不做 agent 佈線，所以不做。可以做的替代：`route` 回報與另一條既有走線的平行間距（例如 `parallel to USB1_N at 0.15 mm for 6.2 mm`），讓代理自己對齊。這是量測，不是佈線。

## 第二優先（原有清單中仍未做）

- `stitch_vias` 的「no free spot」說明是哪些零件擋住。
- `lcsc_search` 的保險絲（hold current）、連接器、ESD（電容、通道數）參數。
- `audit` skew 可排除 TVS 的分支。
- `silk_tidy` 加最大距離或隱藏 reference（第五輪：38 個 reference 找不到空位）。
- 刪除孤立 label（`sch_delete labels=[[x, y]]`）。
- `auto_place` 依腳位綁定去耦電容。
- `lib_create_footprint` 加測試點 / 焊墊種類。
- `sync_pcb` 重複回報 fabrication attribute。
- `lcsc_match replace=true` 的標題字樣。
- `route_short` 的 via 搜尋時間上限（目前固定 45 秒）可否給參數；回應列出跳過的連線數。
- `move_footprints` 給 `parts` 時要求 `refs`：重現不出來，可能是子代理 client 端的舊 schema。

## 不做

- SKILL.md 的「密集兩層 QFP 佈線食譜」（腳位對連接器的映射、IC 本體下的過孔陣列、先逃線再放去耦）：只對 StickHub 這類板子有用，見上方〈方向〉。

## 驗證流程（同 beta.9）

1. **建置：** `ninja -j4 -C build-lean kicad-cli pcbnew_kiface eeschema_kiface kicadd-mcp`。
2. **TOOLS.md：** 工具說明有改動時重產（`build-lean/kicadd-mcp --tool-docs > skills/kicadd/TOOLS.md`）。
3. **全部測試：** `python3 -I client/run_all.py`，36 個。
4. **推送：** push main，等 Windows CI 綠燈，再打 annotated tag。

## 實驗環境備忘

- **第五輪的專案：** `run/exp/ai-hub5/`（完成版，45 條未佈線）與 `run/exp/ai-hub5-crashed/`（第一次跑時伺服器當掉，只有電路圖）。
- **壓力重現腳本：** 在 `ai-hub5` 的副本上做 sync_pcb → auto_place → 三輪 route_short（每 6 顆零件一批）+ fanout。曾重現 `VIA::PushoutForce` 當機與 120 秒逾時。
- **服務啟動指令：** 同 `docs/NEXT_beta9.md`；用完要停，用 pid kill。
