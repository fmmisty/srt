## ============================================================
##  MicroZed 7020 (XC7Z020) 音声SRT STL キャリア ピン制約
##  JX1 Bank34 に集約 / IOSTANDARD = LVCMOS33 (VCCO_34 = 3.3V 前提)
##  MCLK は MRCC ピン (U18 / JX1-42)
##  ※ EMIO(I2C/SPI) のポート名は各自の Block Design のトップ名に合わせて変更
## ============================================================

## ---- クロック : XO 24.576MHz ----
set_property -dict {PACKAGE_PIN U18 IOSTANDARD LVCMOS33} [get_ports mclk_in]        ;# JX1-42 IO_L12P_T1_MRCC_34
create_clock -name mclk -period 40.690 [get_ports mclk_in]                          ;# 24.576 MHz (=512fs@48k)

## ---- I2S (PL) ----
set_property -dict {PACKAGE_PIN T11 IOSTANDARD LVCMOS33} [get_ports i2s_bclk]       ;# JX1-11  IO_L1P_T0_34
set_property -dict {PACKAGE_PIN T12 IOSTANDARD LVCMOS33} [get_ports i2s_lrclk]      ;# JX1-12  IO_L2P_T0_34
set_property -dict {PACKAGE_PIN T10 IOSTANDARD LVCMOS33} [get_ports i2s_sdata_tx]   ;# JX1-13  IO_L1N_T0_34 -> CS4272 DAC + AK4104
set_property -dict {PACKAGE_PIN U12 IOSTANDARD LVCMOS33} [get_ports i2s_sd_adc]     ;# JX1-14  IO_L2N_T0_34 <- CS4272 ADC
set_property -dict {PACKAGE_PIN V12 IOSTANDARD LVCMOS33} [get_ports i2s_sd_aes]     ;# JX1-18  IO_L4P_T0_34 <- CS8422

## ---- I2C (PS EMIO) : CS4272 / CS8422 ----
set_property -dict {PACKAGE_PIN W13 IOSTANDARD LVCMOS33} [get_ports i2c_scl_io]     ;# JX1-20  IO_L4N_T0_34
set_property -dict {PACKAGE_PIN V13 IOSTANDARD LVCMOS33} [get_ports i2c_sda_io]     ;# JX1-19  IO_L3N_T0_DQS_34

## ---- SPI (PS EMIO) : AK4104 ----
set_property -dict {PACKAGE_PIN T14 IOSTANDARD LVCMOS33} [get_ports spi_sclk]       ;# JX1-23  IO_L5P_T0_34
set_property -dict {PACKAGE_PIN P14 IOSTANDARD LVCMOS33} [get_ports spi_mosi]       ;# JX1-24  IO_L6P_T0_34
set_property -dict {PACKAGE_PIN T15 IOSTANDARD LVCMOS33} [get_ports spi_miso]       ;# JX1-25  IO_L5N_T0_34
set_property -dict {PACKAGE_PIN R14 IOSTANDARD LVCMOS33} [get_ports spi_csn]        ;# JX1-26  IO_L6N_T0_VREF_34

## ---- GPIO / 状態 ----
set_property -dict {PACKAGE_PIN Y16 IOSTANDARD LVCMOS33} [get_ports cs8422_lock]    ;# JX1-29  IO_L7P_T1_34 (in)
set_property -dict {PACKAGE_PIN W14 IOSTANDARD LVCMOS33} [get_ports status_led]     ;# JX1-30  IO_L8P_T1_34 (out)
set_property -dict {PACKAGE_PIN Y17 IOSTANDARD LVCMOS33} [get_ports codec_rst_n]    ;# JX1-31  IO_L7N_T1_34 (out)

## ============================================================
##  メモ
##   - VIN_HDR (JX1-57..60 / JX2-12,57..60): キャリアから 5V を供給して MicroZed を駆動
##   - VCCO_34 (JX1-78..80): Bank34 I/O 電圧 = 3.3V に設定（コーデックI/Oと整合）
##   - U13 (JX1-17, IO_L3P..PUDC_B_34): config時特殊機能のため未使用
##   - MCLK は MRCC(U18) に配置済み → MMCM/BUFG へ素直に入力可能
##   - 差動が必要になった場合、JX1 の L*P/L*N ペアで LVDS も可（現状は全て単相CMOS）
## ============================================================
