# 交接：beta.9 發版前的修正（2026-11-22）

此文件是給下一個對話用的工作清單：先修下列項目，跑完測試後就切 `v0.1.0-beta.9`。
**不要把範圍外的項目塞進這版拖延發布。**

## 目前狀態

- `main` 最新為 `21d762e`（Step 3 與 Run 2 修正），Linux 36 個 e2e 全過，Windows CI 綠燈。尚未打 tag。
- `ci/release-notes.md` 已有「這一版（beta.9）」段落，`ROADMAP.md` 已有「盲測」段落（第一、二輪）；發版前把第三輪與本文件的修正補進去。
- 三輪盲測（子代理只依規格設計 7 埠 USB hub，與 KiCad demo 的 StickHub 比較）：

  | | Run 1 | Run 2 | Run 3 | 真人 |
  |---|---|---|---|---|
  | ERC | 0 錯 / 117 警告 | 0 / 0 | 0 / 0 | 0 / 0 |
  | 未佈線 | 96 | 54 | 41 | 0 |
  | USB 差分對佈線 | 2 / 8 | 8 / 8 | 6 / 8 | 8 / 8 |

  Run 3 的專案在 `run/exp/ai-hub3/`。它少佈的兩對是擺件時把連接器放到底面，造成 P/N 反序；`audit` 的 pair order 能抓到，但子代理擺完件後沒有跑。

## 這版要做的事

### 1. `sch_move` 默默改接網路（最嚴重，阻擋發版）

**現象（Run 3）：** 在 Ports 圖紙上執行 `sch_move {ref:"U5", x:140, y:134}`，接著 `{ref:"U6", x:140, y:180}` 之後：
- U5 的 1、6 腳掉網。
- U6.3 變成 USB5_N，U6.4 變成 USB6_P。
- 留下一個懸空的 USB7_P 標籤。

回應沒有任何提示，事後跑 ERC 才看出來。

**原因方向：** KiCad 原理圖以 `(x, y)` 座標重合判定連線。元件移到密集區時，腳位、stub 端點或標籤很容易剛好落在旁邊的標籤或導線上而併網。

**程式位置：** `src/sch/kicadd_handler_sch.cpp` 的 `KICADD_HANDLER_SCH::handleMove`（約 2418 行）。它把每支腳的標籤、stub 收起來，移動元件後再放回新位置；要拖動的導線放在 `drag` 清單。

**修法：**
- **比對網路：** 移動前記錄該元件（所有 unit）每支腳的網路名稱。移動後 `rebuildConnectivity()` 再取一次，逐腳比對。同時檢查其他元件的腳是否因此改網：至少比對移動後新位置附近（受影響範圍內）其他零件的腳。
- **預設拒絕並回滾：** 只要有腳掉網、改接別的網，或別的元件的腳被改接，就拒絕這次移動、整個回滾（commit 不 push，或 revert）。回應要列出完整 diff，例如 `U6.3: NC -> USB5_N`、`U5.1: USB4_P -> (none)`。
- **`allow_rewire`：** 新參數，預設 `false`。`true` 時照做，但回應一樣列出 diff。
- **同一套檢查：** `sch_wire`、`sch_add_part`（給 x / y 時）也可以用。至少 `sch_add_part` 放到別的腳上時要警告（Run 1 也回報過）。

**測試：** 放在 `client/sch_edit_e2e.py` 或 `client/project_e2e.py`。
1. 做一個密集情境：把零件移到別的零件標籤或 stub 端點上，或讓其腳端點落在別的網路的導線上。
2. 預設應拒絕，回應附 diff，圖不變（`sch_netlist` 前後相同）。
3. `allow_rewire=true` 時照做並列出 diff。
4. 一般移動（不撞任何東西）照常成功。

### 2. 用 `lib_id` 放不同值的零件時沒有警告

**現象（Run 3）：** `sch_add_part {lib_id:"LCSC:CL05B104KO5NNNC", value:"4.7uF"}` 回應寫 `(C1525)`，沿用了 100 nF 零件的 LCSC 料號、MPN 與 Description，卻沒有警告。Step 3 加的警告沒有觸發。

**程式位置：** `mcp/sch_tools.cpp` 的 `sch_add_part` lambda（約 851 行起；警告在約 920 行，`warning: its LCSC number ...`）。

**要查：**
- `r.symbol().lcsc()` 是否為空。回應中的 `(C1525)` 可能來自別的欄位，這時要改從 fields 讀 LCSC。
- `LcscSearch` 以料號查詢時，`hits(0).value()` 是否為 0（電容的 `value` / `unit` 有沒有填）。
- 值 `4.7uF` 的解析：取第一個字，用 `sim::ParseSI`。

**順便：** `sch_set_fields` 改 `LCSC` 時，也要像 `lcsc_match` 一樣改寫或清除 Supplier Part / Manufacturer Part，以及提到舊料號的 Description / Datasheet。可以抽出共用函式，在 `lcsc_match` 的 apply 迴圈裡已經有寫法。

**測試：** 放在 `client/match_e2e.py`（有 parts index）。以已有 LCSC 的符號、用不同值放置，應出現警告；`sch_set_fields` 改 LCSC 後，Supplier Part 等欄位要跟著改。

### 3. USB-C 雙焊盤造成 pair order 誤報

**現象（Run 3）：** `audit` 的 pair order 把上游差分對（U1–J1、U2–J1）判為反序。USB-C 正反插各有一組 D+ / D-（A6/B6、A7/B7），同一網路在同一顆零件上有兩個焊盤。

**程式位置：** `src/pcb/kicadd_lengths.cpp` 的 `// ---- pair order` 區塊（約 459 行）。目前每顆零件只取第一個 P 焊盤與第一個 N 焊盤。

**修法：**
- 同一零件上同網路有多個焊盤時，改用以下其中一種做交叉判斷，都能消掉這個誤報：
  - 同網路焊盤群的質心（centroid）。
  - 對每對零件，取最接近對方零件的那一個焊盤。
- 建議用後者，因為它反映實際走線會接的那一組。

**測試：** 放在 `client/length_e2e.py`。用一個有 USB-C（雙焊盤）的板子：Run 3 的板子不能進 repo，可以在測試裡用 `add_footprint lcsc=C165948` 搭配 LCSC 快取，或直接構造兩顆零件的焊盤位置。不應報反序；真的反序的情況仍要報。

### 4. 擺件後自動回報 pair order

- **在哪裡回報：** `move_footprints`、`auto_place` 的回應，只要被移動的零件有差分對的焊盤，就附一行 pair order。只列和這次移動有關的差分對；沒有反序時一行「pair order: ok」或乾脆不寫，維持省 token。
- **怎麼實作：** 把 `kicadd_lengths.cpp` 的 pair order 計算抽成可共用的函式。在 handler 的 `handleMoveDelta` / `movePoses` / auto_place 結果組裝處呼叫，寫進 `MoveResponse.notes` 或 `AutoPlaceResponse` 的 notes。
- **SKILL.md：** 補一段完整的例子：底面連接器對頂面 IC 的腳序會鏡像，擺完件就看回應裡的 pair order。

### 5. 第 4 點裡順手帶進的兩個小修正

- **`route_short` 把 via 打在 QFN 本體底下**（Run 3：U1 的 pad ring 與 EPAD 之間）。底下有散熱焊盤，打進去不是短路就是吃錫不良。
  - **怎麼修：** 在 `src/pcb/kicadd_handler_pcb.cpp` 的 `handleRouteShort` 中，via fallback（`auto tryStops`，約 2942 行）產生 spot 時，排除落在任何多腳 IC（焊盤 ≥ 8 或有 EPAD）的焊盤外框（pads 的 bbox 或 courtyard）內的點。只改 spot 篩選，**不要**改路由器本身。`route`（手動）不變。
  - **範圍：** 若要動到 PNS 規則或路由器本身，就改放 beta.10。
- **`route_short` 接受不存在的網路名稱**（回 `" ->  [VDD18]: not routed: no such net"`）。改成整個請求以參數錯誤拒絕，並列出相近的網路名稱。位置在 `handleRouteShort` 約 2770 行；或在 MCP 端先查。

## 不在這版（beta.9 之後）

- 非直線的 via 搜尋（目前只沿兩端直線、±0.8 / 1.6 mm 偏移試 25 個點）。
- 各工具未連線數統計對齊（`stitch_vias` / `route_short` / `audit` / `move_footprints` 的數字不同，且沒說排除了哪些網路）。
- IC 內部 1.8 V 穩壓輸出（如 USB2517 的 VDD18、VDD18PLL）的腳位型別推定。
- 刪除孤立 label（`sch_delete labels=[[x, y]]`）。
- 其他 Run 3 的建議：
  - `silk_tidy` 加最大距離或隱藏 reference。
  - `sch_part` / `sch_netlist` 顯示 no-connect。
  - `lcsc_search` 比對封裝名稱中的字、接受頻率值。
  - `auto_place` 依腳位綁定去耦電容。
  - `lib_create_footprint` 加測試點 / 焊墊種類。
  - `sync_pcb` 重複回報 fabrication attribute。
  - `batch` 遇到 DRC 警告不停。
  - `lcsc_match replace=true` 的標題字樣。
  - pair 交叉的輔助工具。
  - QFN 本體禁區做成正式的路由規則。

## 驗證與發版流程

1. **建置：** `ninja -j4 -C build-lean kicad-cli pcbnew_kiface eeschema_kiface kicadd-mcp`（-j16 會把容器記憶體用光）。
2. **TOOLS.md：** 工具說明有改動時重產 `build-lean/kicadd-mcp --tool-docs > skills/kicadd/TOOLS.md`；`tools_e2e` 會檢查它是否最新，也檢查說明長度上限（描述 < 200 字元、參數描述 ≤ 60、整份 < 40k）。
3. **全部測試：** `python3 -I client/run_all.py`，36 個，約 25 分鐘，可用 `--only a,b`。沒有 `--keep-going`。
4. **MSVC：** 不要用 `near` / `far` 當識別字（windows.h 巨集），beta.3 與 `45efb4a` 都因此在 Windows 失敗。
5. **推送：** push main，等 Windows CI（約 25–30 分鐘）綠燈。
6. **文件：** 把本次修正與第三輪結果補進 `ci/release-notes.md` 的 beta.9 段落與 `ROADMAP.md` 的盲測段落；工具數仍為 64。
7. **打 tag：** 建 annotated tag `v0.1.0-beta.9` 並 push。CI 依 `ci/release-notes.md` 發佈 pre-release（含 `kicadd-windows-x64.zip`、`-pdb.zip`、`kicadd-skill.zip`）。

## 實驗環境備忘

- **啟動 MCP 服務：**
  ```
  KICADD_BUILD=$PWD/../build-lean KICADD_PARTS_DB=$PWD/run/exp/parts.db \
  KICADD_NGSPICE=$PWD/run/sim/ngspice/usr/bin/ngspice KICADD_LCSC_CACHE=$PWD/run/lcsc-cache \
  client/kicadd_up.sh run/exp/seed/seed.kicad_pcb 8765
  ```
  用完要停。用 `ps` 找 pid 後 kill；不要用 `pkill -f` 的樣式，它會連自己的 shell 一起殺掉。
- **服務重啟後：** Claude Code 的 MCP client 可能仍是舊的工具 schema；伺服器已會把字串參數依 schema 轉型。
- **重建前先停服務：** 服務執行中重建 kiface，可能讓服務當掉；需要時只編單一 `.o`。
