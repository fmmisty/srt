# XMOS XU316 実ピン割当(案) ― KiCad回路図用

対象: U1 = XU316-1024-FB265 (xcore.ai, 265-ball FBGA, 2タイル)。
`schematic_notes.md` の機能と対応。**xcoreリソース(ポート)＝機能割当を確定**し、実ボール番号(★)はFB265データシートのピンリストで最終確定する。
実装は XMOS `sw_usb_audio`(`lib_xua`)ベース、`xua_conf.h` の各定義を本表に合わせる。

## 1. タイル分担

| タイル | 用途 |
|---|---|
| tile[0] | USB(統合PHY)、I2C(コーデック制御)、前面OLED/LED、リセット/ブート、電源 |
| tile[1] | I2S(ADC/DAC)、SPDIF TX/RX(AES=SRC4392経由)、MCLK入力、クロック選択 |

## 2. 機能 → xcoreリソース割当(案)

| 機能 | 信号 | xcoreリソース(案) | 方向 | 接続先 | 備考 |
|---|---|---|---|---|---|
| USB | USB_DP/DM, VBUS, ID | 統合USB(専用ピン) | ⇔ | CM4 USB2.0 | xcore.ai内蔵PHY。90Ω差動 |
| MCLK入力 | MCLK | 1bit port(clock in) | in | クロックバッファ | 24.576/22.5792、`lib_xua` clockgen |
| I2S BCLK | BCLK | 1bit port | out | PCM1862/PCM5242/SRC4392 | デバイスマスタ |
| I2S LRCLK | LRCLK/WS | 1bit port | out | 同上 | |
| I2S ADC data | I2S_ADC_SD | 1bit port | in | PCM1862 SDOUT | 24bit |
| I2S DAC data | I2S_DAC_SD | 1bit port | out | PCM5242 SDIN | 24bit |
| SPDIF/AES TX | SPDIF_TX | 1bit port | out | SRC4392(TX側/I2S) | AES送信。実装でI2S直結かSPDIFか確定 |
| SPDIF/AES RX | SPDIF_RX | 1bit port | in | SRC4392(RX側/I2S) | AES受信 |
| I2C SCL | I2C_SCL | 1bit port(オープンドレイン) | out | PCM1862/PCM5242/SRC4392/OLED | プルアップ2.2-4.7k |
| I2C SDA | I2C_SDA | 1bit port(双方向) | ⇔ | 同上 | |
| クロック選択 | CLKSEL | 1bit GPIO | out | クロックバッファ/セレクタ | 48k系/44.1k系切替 |
| コーデックRST | CODEC_RST# | 1bit GPIO | out | PCM1862/PCM5242/SRC4392 | 初期化制御 |
| OLED(I2C共用) | (I2C) | 上記I2Cバス | ⇔ | SSD1306 | アドレス0x3C |
| LED | LINK/LOCK/SIG/CLIP | 多bit port or GPIO | out | 前面LED | 4-5本、+抵抗 |
| XMOS BOOT | ブートモード | ブートピン | — | フラッシュ/リンク | QSPIフラッシュ要否は実装で確定★ |
| XMOS DEBUG | xTAG/JTAG | 専用 | ⇔ | デバッグヘッダ | 開発用 |

## 3. 電源ピン

| レール | ピン群 | 供給 | 備考 |
|---|---|---|---|
| VDDcore +0.9V | VDD | REG3(専用Buck) | 低リップル、DS値で確定★ |
| VDDIO +3.3V | VDDIO | REG2 | I/O |
| USB/PLL | 該当VDD | 分離/フィルタ | ジッタ低減 |
| GND | 多数 | GND | 熱パッド/ビア |

## 4. I2S/クロック方針(再掲)

- デバイス(XU316)を**マスタ**、MCLKはローカルXO。ADC/DAC/SRC4392(TX)は従属。
- **AES INのクロックずれは SRC4392 の ASRC で吸収**(XU316側は常にローカル同期)。
- I2Sは24bit。サンプルレート48/96kは`lib_xua`のsample-rateで切替、CLKSELでXO系統選択。

## 5. 次工程

1. FB265データシートのピンリストで**各リソースの実ボール番号を確定**(本表の「案」を埋める)。
2. `xua_conf.h`(ポート定義)を本表に一致させる。
3. KiCadでXU316シンボル(ピン=本表)＋周辺(デカップリング/PLLフィルタ/ブート)を描く。
4. 他IC(PCM1862/PCM5242/SRC4392/CM4/電源)を結線 → DRC → レイアウト。
