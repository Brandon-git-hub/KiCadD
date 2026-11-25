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
