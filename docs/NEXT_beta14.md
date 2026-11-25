# 交接：beta.14 的工作清單（2026-11-24）

來源：beta.13 階段 3 的審查型盲測 R1（Jetson AGX Thor baseboard，報告 `run/exp/r1-review.md`，摘要在 `docs/NEXT_beta13.md` 文末）。beta.13 已處理其中五項（見 NEXT_beta13〈階段 3 之後的修正〉）；以下是留到 beta.14 的。原則不變：只做通用改進。

## 第一優先

1. **`audit ir_drop` / `pdn` 穿過 0 Ω 跳線與分流電阻。**
   - R1 的情形：
     - pdn 在 +1V15 停在 R7（0R 到 /DCDC/1V15_OUT），穩壓器的輸出電容沒有算進去，241 mΩ 的峰值是假的。
     - ir_drop 的總覽說 +1V15 / +5V / VCC / +1V8「no source part」，因為這些軌都經 0R 或分流電阻供電。
   - 作法：把小於約 0.1 Ω 的電阻（0R、分流電阻）當成導通，沿著它走到另一側的網路去找電源和電容。走過了哪些電阻，回應要列出來（以及它們的電阻值）。
2. **`audit ir_drop` 預設的 source / load 推定。**
   - R1 的情形：
     - 16V_OUT 把 INA219 的分流 / 量測電阻（R364 / R65 / R69）當成負載，每顆 8 A，報出 564 mV 和一顆 0.1 mm 量測線過孔 5.33 A，是誤報。
     - +5V_SOM 把 SoM 連接器 J1（其實是負載）當成電源。
     - 「no copper to the source: R61.2」其實有經過孔接到。
   - 作法：
     - 大於約 10 Ω 的電阻不當負載。
     - 連接器在其他路徑上找得到穩壓器時，不當電源。
     - 推定結果要寫出依據。
3. **`audit current` 在鋪銅網路上的判斷。** 它用最細的走線（0.1 mm sense / stub）比 IPC-2221，鋪銅承載的軌（VCC、SYS_VIN_HV、16V_OUT）因此全被標出來。作法：
   - 網路主要靠鋪銅時，改報鋪銅的最窄截面，或交給 ir_drop。
   - 只連到一個焊盤的 stub，另外列出來。
4. **`sch_summary` 的篩選與未連接腳清單。**
   - R1 的 sch_summary 無參數回了 157 kB。要加 `sheet` 和 `limit`，或者只回標頭。
   - 標頭寫「47 unconnected pins」，卻沒有列出是哪些。至少要列出前 N 支（REF.PIN，名稱，sheet）。
   - 用屬性列零件：DNP、exclude_from_bom、value / footprint glob。R1 必須解析 `.kicad_sch` 才找到 Q2 / Q9 / Q39。
   - 94 個階層 bus 腳標為「(not connected on /)」，看起來是 root 側的 bus 沒有展開，要查是否誤報。
5. **從 net class 名稱推定阻抗目標。** 這塊板的 class 名稱是 `85Ohm-diff_PCIE`、`90Ohm-diff_USB_2.0`，`audit impedance` 沒有 z_diff_target 就不判斷；agent 一開始猜錯目標。作法：
   - class 名稱裡有 `<n>Ohm` / `<n>ohm` 且是 diff class 時，當成建議目標寫進標頭，例如「targets from class names: 85 Ω ...」。
   - 只有給了 `z_diff_target` 才做判斷，維持「不給門檻不下結論」的原則。

## 小問題（措辭與一致性）

- `audit` 的參數叫 `currents`，對應的 section 卻叫 `current`。section 也接受 `currents`（或者在錯誤訊息裡建議）。
- `impedance` 把有具名 prepreg（PR2116 / PR1080，各有 Er）的疊構稱為「generic (not an impedance-controlled stackup)」。要改寫，或說明判斷依據。
- `impedance` 的每層表格把平面層也列進去（In1.Cu 本身是 94 % 平面，卻寫「embedded microstrip ... no plane on the other side」）。平面層應該略過，或另外標示。
- `audit` 的 power / decoupling / net 沒問題時不輸出任何東西。要各給一行「OK」。
- `audit diff_pair` 的 USB-C D± skew 包含 A/B 兩排之間的橋接 stub（6–9 mm），要說明，或者量經過的路徑。
- `skew` 的 glob 比對的是 pair 名稱（`USB0.D+/-`），`*_D_N` 之類的寫法比對不到。沒有比對到時，要列出幾個實際的 pair 名稱當例子。
- `length_match` 把 AC 耦合電容兩側的網路（DP0.TX 5 mm 和 DP0.TX_C 83 mm）分開比較。同一組裡長度差很多時，要提示「是不是分段？」。
- `audit xtalk_length` 的寬 glob 會抓到慢速 sideband（CLKREQ / RST / I2C）。可以考慮「只看 diff class 裡的網路」這個選項。
- `sch_part` 沒有腳的篩選（J1 有 700 行）。加 `pins` 或 `net` glob。
- `query_area` 的零件列沒有 value。
- ERC「multiple_net_names」引用 #PWR 的隱藏腳時，名稱和 symbol 的 value 不一樣（KiCad 本身的行為），可以補充說明。

## 已處理：Marlow（DM0002，RP2040 + TLV758P）研究回報（2026-11-25）

- **simulate 自動電源與 subckt 衝突：** subckt 的 power_out 腳所在的網路算「區內產生」，不再依名稱補電源。`sources {net: null / "none"}` 可以明確關掉某條網路的電源。
- **內建 LDO 積分飽和：** 誤差放大器輸出鉗在 0 .. Vin − dropout（EN 關時為 0），VIN 從 0 V 起的 tran 不再卡在 Vin − dropout。加上限流：`ilim`，沒給時用資料的 Iout × 1.5（標 assumed），兩者都沒有時說明無限流。`models.{ref}.soft_start`（秒，資料手冊的啟動時間）：參考電壓從 EN 開啟且 Vin 高於 `uvlo`（預設 1 V，assumed）起線性爬升；沒有給時間但零件描述列出 Soft Start 時，模型說明會講明未模擬、啟動湧入電流被高估。電流到達限流值時，輸出列出「at its current limit … for …」。導通元件只供電不吸電（反向 1 mS），PSRR 饋通只在導通時作用（關閉或 soft start 剛開始時不把輸入的跳變帶到輸出）。Marlow 原報告的 15 顆（jlcparts 2026-11-21 的 C2876308 資料）：舊版 4.885 V、峰值 2.4 A；新版 3.149 V、峰值 655 mA（未達 750 mA 限流）；soft_start 1 ms 時 55 mA。
- **內建運放：** 輸入加 ESD 二極體到兩軌（`input_clamp 0` 關閉；描述寫 power-off / fail-safe 的零件預設關閉並說明）。斷電時輸出上下限交會在兩軌中點，不再從 0 V 電源輸出 1.2 V。
- **網路名稱：** 板子工具以名稱找網路時，接受有或沒有開頭 `/` 的寫法（`kicadd::FindNetLoose`：query_net、board_render、topology / reflection、pdn、teardrops、stitch_vias 等）。
- **reflection：** 沒有 output 腳時改用 bidirectional 腳當 driver（先找串聯電阻後面），並在輸出說明；板子沒有腳的類型時（從別的工具來的板）用電路圖的。最遠接收端的 TD 改在未合併的節點上算（rise 慢時短線段被併成電容，原本算成 0 s）。
- **小項：** `models` 裡沒有作用的鍵會列出（零件被略過時提示 `kind`）；新增 `models.{ref}.kind`（ldo、opamp、r、c、l、d、zener、mosfet、bead），給沒有 LCSC 編號的零件用內建模型。圖表刻度的位數依刻度間距決定。
- **順手修：** 值為 `0` / `0R` 的電阻當 1 mΩ 跳線（原本被略過、網路斷開）；預設的 probes 遇到沒有模型零件的網路時略過，不再整個失敗。

## 根本改進計畫（2026-11-25 定案，另開會話實作）

背景：Marlow 回報的模型與工具問題零散，改為系統性做法，不再逐項修回報。範圍以內建模型為主；不做外部廠商 SPICE 模型的自動取得（subckt 維持現狀）。

### 一、內建模型的情境測試與物理合理性檢查（優先）

**1a. 固定參數情境測試 `client/model_e2e.py`（加入 run_all，headless）**

- 空白專案，用 `sch_*` 工具建小電路（同 `sim_e2e` 的做法）。參數用 `kind` + `models` 固定，不依賴 parts index；每類另加一個讀 index 的案例，涵蓋資料路徑。
- 情境矩陣：

  | 情境 | LDO | 運放 | MOSFET N/P | 二極體 / 齊納 | R / C / L / 磁珠 |
  |---|---|---|---|---|---|
  | Vin 從 0 V 起動 | ✓ | ✓ | ✓（閘極驅動） | ✓ | ✓（RC / LC 充電） |
  | EN 開關 | ✓ | — | ✓（當開關） | — | — |
  | 負載步階 | ✓ | ✓ | ✓ | — | — |
  | 輸入步階 | ✓（line transient） | ✓（slew） | — | ✓ | ✓ |
  | 過載、限流 | ✓ | ✓（輸出） | ✓ | ✓（順向） | — |
  | 斷電（供電 0 V、輸入仍被驅動） | ✓ | ✓ | ✓ | — | — |
  | 反向偏壓 / 反向電流 | ✓（Vout > Vin） | — | ✓（body diode） | ✓（反向、齊納） | ✓（有極性電容） |

- 只檢查物理界線，不比對精確數值：輸出在電源軌之內、不卡在 Vin − dropout、電流不超過限流、關閉時不輸出、在說明的時間內穩定。
- 估計 700–900 行 Python，40–50 項檢查。

**1b. 批量實測腳本 `client/model_survey.py`（不進 run_all，release 前跑）**

- 每類從 parts index（有 params 表的版本）抽樣真實零件，預設每類 100 顆（`--per-class`，50–200），固定 random seed，結果可重現。類別：LDO、運放、N / P MOSFET、二極體、齊納、MLCC、電解 / 鉭 / 高分子電容、電感、磁珠、電阻。
- 每顆零件放進該類的標準電路（通用方塊符號 + LCSC 欄位），跑 1a 的同一套情境。判定只用 1c 的自動物理檢查與 1a 的界線，不做設計判斷。
- 輸出每類統計（`run/survey/<日期>/`，Markdown + JSON）：抽樣數、成功建模數、被略過的原因分布、各項檢查的觸發次數與比例、觸發最多的代表零件（LCSC、描述、哪個情境、哪項檢查）。
- 觸發過問題的代表零件，修正後固定進 `model_e2e` 當回歸案例（參數寫死在測試裡，不依賴 index 版本）。
- release 流程加一步：跑 `model_survey`，與上一次的統計比較；異常比例上升要先查。

**1c. `simulate` 每次跑完的自動物理檢查（`mcp/sim.cpp`）**

- 違反時在結果加 `model check: …` 一行（附數值），只標「這個結果可能是模型造成的」，不下設計判斷。門檻固定（已確認），並寫在警告行裡。
- 檢查項目：
  1. 調節器輸出高於輸入超過一段時間（反向導通或模型錯誤）。
  2. 0 V 的電源輸出功率。
  3. 電流解釋不了：某個電源（或調節器）的電流超過宣告負載加調節器額定 Iout 的 3 倍，或電源在吸電（兩個源互打，如 Marlow 的 66 A）。SPICE 的 KCL 一定成立，所以不是做加總比對。
  4. 調節器輸出在視窗內超過 20% 的時間停在 Vin − dropout，而 Vin 已高於設定值 + dropout（windup 的特徵）。
  5. 導通元件（LDO 導通元件、MOSFET、二極體）有超過漏電等級的反向電流。
  6. 運放輸出超出電源軌，或電源為 0 V 時仍在輸出電流。
- 為此每次多存所需的向量（電源電流、調節器輸入 / 輸出 / 導通電流、二極體與 MOSFET 電流）。先量測對執行時間和輸出長度的影響。
- 估計 300–400 行。

**1d. 模型未涵蓋的行為一律說明**

- 每個內建模型有一份固定的「未模擬」清單：行為、影響、用哪個鍵補。例：
  - LDO：沒給時間的 soft start、熱關斷、輸出放電、反向電流保護。
  - 運放：輸入共模範圍、輸出限流、相位反轉。
  - MOSFET：level 1、無溫度、無雪崩。
  - 二極體：反向恢復、溫度。
  - MLCC：溫度、老化。
- 輸出每顆零件一行簡短的 `not modelled: …`（維持 token 精簡），完整說明放 `tool_help simulate`。
- 這份清單與 2b 的 models 鍵登錄是同一份定義。
- 估計 150 行。

### 二、共用的正規化與參數驗證

**2a. 共用的查找與正規化層**

- 網路名稱：純 C++ 的 header-only `src/common/kicadd_names.h`，KiCad 端（板子與電路圖 handler）和 kicadd-mcp 都引用，規則只有一份：
  1. 完全相同
  2. 加上或去掉開頭的 `/`
  3. 葉名稱（最後一個 `/` 之後）唯一時
  4. 不分大小寫唯一時
  - 多個符合時報錯並列出候選；找不到時建議最接近的 3 個。
- 要改的地方：板子 handler 約 20 處以名稱找網路（含 `FindNetLoose`）、電路圖 handler 的網路解析（需盤點）、kicadd-mcp 約 24 處（`simulate`、`reflection`、`pdn` 等的 `findNet` / `bare()`）、6 個檔案裡的 glob 比對（也套用葉名稱與大小寫規則）。
- 腳位類型：kicadd-mcp 一個共用查詢，板子的 pad 沒有類型時用電路圖的，每次呼叫快取。server 和 desktop（有開電路圖時）都適用。目前只有 `reflection` 用到；之後其他工具要讀腳位類型也走它。不放在 handler 裡，因為單開的 PCB 編輯器沒有電路圖可查。
- 估計 header 250 行、改約 60 個呼叫點、測試約 150 行（`tools_e2e` 或新的 `names_e2e`）。

**2b. 工具參數由一份參數表定義**

- 現況：伺服器已經依 schema 拒絕未知的頂層參數名稱；型別、範圍和 SI 解析是各工具各自寫的（`jsonSI` / `is_number` 約 100 處）；`models`、`nets` 這類自由物件裡的鍵沒有檢查。
- 新做法：`mcp/params.h` 每個參數宣告一次：名稱、型別（數值 / SI / 字串 / enum / bool / 陣列 / 物件）、單位、範圍、預設、簡短說明。由它產生：
  - inputSchema
  - 執行前的型別、範圍、enum 檢查（各工具錯誤格式一致；例如字串多了一層引號的 `"\"4n\""` 會得到明確訊息）
  - 巢狀物件裡的未知鍵錯誤
  - `tool_help` / `TOOLS.md` 的參數段落
- `simulate` 的 `models`：依模型類型登錄可用的鍵。任何模型都不接受的鍵 → 錯誤；合法但這顆零件的模型用不到的鍵 → 提示。取代本輪的執行期追蹤（`PART::read`），與 1d 共用來源。
- 一次遷移全部 58 個工具（已確認），避免兩種驗證方式並存。框架約 600–800 行，工具改動約 1,500–2,000 行（主要在 `tools.cpp`、`sch_tools.cpp`）。
- 風險：每個工具都會動到；`tools/list` 的大小不能變大（前後比較 token 數）；Windows CI。

### 順序與驗證

1. 一：1a → 1c → 1d → 1b（先有固定情境，再加自動檢查與說明；批量實測用前面三項判定，放最後）。
2. 2a。
3. 2b：先框架加 `simulate`、`reflection`、`pdn`（參數最複雜），其餘工具分批遷移。

- 每一步完成跑完整的 headless 測試，push（不 release）並看 Windows CI。每部分結束跑一次桌面測試。
- 完成的項目在本節打勾。

### 已確認（2026-11-25）

- 1c 的門檻採固定值，每個門檻寫在它的警告行裡；不提供每次呼叫指定。
- 2b 在 beta.14 一次遷移全部 58 個工具到參數表，不留舊的驗證方式。
