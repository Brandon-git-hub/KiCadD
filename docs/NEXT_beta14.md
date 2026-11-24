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
