# CM4キャリア基板 概略回路 ネット/ピン注記

図: `schematic_block.svg`。本書は結線の要点と、詳細回路図(KiCad)化のときに効く注意点。

## 1. バス一覧(概略)

| バス | 主/従 | 接続先 | 備考 |
|---|---|---|---|
| USB2.0 | CM4(host) ⇔ XU316(device) | 音声4ch(4in/4out) | DP/DM差動、90Ω・長さ整合。CM4のUSB2.0を1本占有 |
| I2S0 | XU316 → PCM1862(ADC) | BCLK/LRCK/SDOUT(ADC→XU) | 24bit I2S。ADCはスレーブ(XU316/クロックがマスタ) |
| I2S1 | XU316 → PCM5242(DAC) | BCLK/LRCK/SDIN(XU→DAC) | 24bit I2S |
| SPDIF/I2S | XU316 ⇔ SRC4392 | AES(RX/TX) | SRC4392でASRC。XU316はlib_spdif or I2S接続のいずれか(実装で確定) |
| MCLK | クロック → XU316/ADC/DAC/SRC4392 | 24.576 / 22.5792MHz | 低ジッタXO。別LDO給電。分配はバッファ推奨 |
| I2C | XU316(master) → PCM1862/PCM5242/SRC4392/OLED | SDA/SCL | プルアップ各2.2k〜4.7k。アドレス重複注意(下記) |
| GPIO | CM4 → FAN(PWM), (UART保守) | — | FANは温度連動PWM。フェイルセーフ=フル回転 |

## 2. I2Cアドレス(要確認・重複回避)

- PCM1862: 0x4A/0x4B(ADR設定)
- PCM5242: 0x4C/0x4D(ADR設定)
- SRC4392: 0x70系(A0/A1設定)
- OLED SSD1306: 0x3C/0x3D
- ※重複が出る場合はADRピンで分離、足りなければI2Cを分岐(XU316の別ポート)する。

## 3. 電源レール

| レール | 供給先 | 生成 | 目安 |
|---|---|---|---|
| +5V(3A↑) | CM4 | Buck | CM4本負荷~1.5A(ピーク~3A) |
| +3.3V(デジタル) | XU316 IO・OLED | Buck/LDO | |
| +0.9V | XU316 VDDcore | 専用Buck(低リップル) | データシート値で確定 |
| アナログ+3.3V(クリーン) | PCM1862/PCM5242 AVDD | 低ノイズLDO | 音質の要、デジタルと分離 |
| ±15V | THAT1246/THAT1646 | DC-DC+LDO | +4dBu/最大~+22dBu |
| クロック用 | XO | 別LDO | ジッタ低減で分離 |

- GND: アナログGND/デジタルGNDは1点(スター)接続。接地は御社電源基板方式(/PE分離・シャーシGND)。
- 各ICはローカルデカップリング(0.1u+10u)。XU316コアは低ESR。

## 4. CM4まわり(キャリア設計の要点)

- **GbE**: CM4はEthernet PHY内蔵。キャリアに**磁気部品(マグネティクス)＋RJ-45**を実装(RGMIIはCM4内部、外部はMDI差動)。
- **USB2.0**: CM4のUSB2.0を**XU316へ直結**(1デバイス)。ハブ不要。
- **USB-OTG(USB-C)**: 保守＋**eMMc書込(rpiboot)**用。nRPIBOOT/USB_OTGの扱いを回路で用意。
- **WiFi**: CM4無線モジュール(WiFi付型番)→**u.FL/MHF4→RF窓内蔵アンテナ**。技適はCM4＋認証アンテナの組合せで担保。
- **起動**: eMMC(SD不要)。
- **給電**: +5V。突入・逆接保護。
- **FAN**: CM4 GPIO(PWM)で制御、温度は`vcgencmd measure_temp`で監視・ログ。

## 5. オーディオ経路(確認)

- ANALOG IN → THAT1246(±15V, バランス受け+アンチエイリアスLPF) → PCM1862(I2S) → XU316 → USB → CM4
- CM4 → USB → XU316(I2S) → PCM5242 → THAT1646(±15V, バランス送り) → ANALOG OUT
- AES IN → 110Ωトランス → SRC4392(RX+ASRC) → XU316 → USB → CM4
- CM4 → USB → XU316 → SRC4392(TX) → 110Ωトランス → AES OUT
- MCLKはローカルマスタ。**AES INのクロックずれはSRC4392のASRCで吸収**。

## 6. 前面表示

- OLED(SSD1306 I2C)＋LED: USB LINK / AES LOCK(SRC4392ロック) / ANALOG SIG・CLIP / dBFS。
- メータはXU316のサンプルフックでピーク検出→OLED/LEDへ。

## 7. 次工程

1. 本注記のピン割当を**XU316実ポート/CM4ピン**に確定(xua_conf.h と対応)。
2. **KiCadで詳細回路図**(各IC結線・デカップリング・保護)。
3. **確定BOM**(型番/員数/リファレンス)。
4. 基板レイアウト(差動長整合・クロック分離・アナログ/デジタル分割)→ 機構(1U/パネル/RF窓)。
