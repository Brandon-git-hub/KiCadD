<!-- Release notes template for tag builds (.github/workflows/windows.yml, job release).
     {{UPSTREAM}} is filled from patches/UPSTREAM_BASE; build info and SHA-256 are appended. -->
KiCadD 試用版：以 KiCad nightly（upstream {{UPSTREAM}}）為基礎的修剪版 Windows x64 build。

> ⚠️ Pre-release，僅供試用。由 GitHub Actions 自動建置，未經人工 Windows 實機測試。

## 安裝 / 執行
1. 下載 `kicadd-windows-x64.zip`，解壓到任意資料夾（免安裝）。
2. 執行 `kicadd-windows-x64\bin\kicad.exe`（專案管理器），或直接開 `bin\pcbnew.exe` / `bin\eeschema.exe`。
3. 執行檔未簽章，Windows SmartScreen 會警告：「其他資訊 → 仍要執行」。

## 這一版拿掉了什麼
| 拿掉 | 影響 |
|---|---|
| OpenCASCADE | 不能匯出 STEP / GLB / STL / U3D；3D 模型只讀 VRML（`.wrl`），STEP 模型在 3D 檢視中不顯示；沒有 Windows 檔案總管的 3D 模型預覽 |
| ngspice | 原理圖沒有模擬器（SPICE 模型欄位與 SPICE netlist 匯出仍保留） |
| Python | 沒有 Python 外掛 |

其餘的原理圖、PCB、推擠走線、差分對、等長、DRC、Gerber 匯出等都是 KiCad 原本的功能。

## 注意
- **沒有附元件庫。** 符號 / 封裝庫是 KiCad 的獨立專案，這版不含；請開你自己的專案（含專案庫），或另外下載 KiCad 官方庫。立創按需抓庫是 M4。
- **想先隨便開個板子試：** `share\kicad\demos\` 裡有 KiCad 的範例專案（例如 `pic_programmer`、`video`），封裝已內嵌在板子裡，不需要庫就能開。
- 設定檔位置與 KiCad nightly 相同（`%APPDATA%\kicad\10.99`），不影響已安裝的正式版 KiCad 9 的設定。
- 介面目前跟 KiCad 一樣；桌面版 Agent 協同（GUI 內接 `.sock` 即時刷新）是 M2，尚未包含。
- 已包含 headless 部分：`bin\kicad-cli.exe api-server` + `bin\kicadd-mcp.exe`（MCP、2D 網頁檢視）。

## CI 驗證（smoke test）
`kicad-cli` 啟動、`pic_programmer` DRC、`rectifier` 匯出 SPICE netlist、API server 經 `kicadd-mcp --stdio` 回應 `board_summary`。
