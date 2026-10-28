# Pico / Pico 2 SBUS → UART → Uno R3 DShot300

既存の `pico-copter-fc` と同じ基板を使い、Pico側でSBUSを読み取ってArduino Uno R3へ送る**プロペラなしのESC/モーター単体試験用**ファームウェアです。姿勢制御は行いません。

## ダウンロード

GitHub Actions **Build Pico and Pico 2 UF2** の実行結果から、利用するボードのArtifactsを取得します。

- 通常のRaspberry Pi Pico（RP2040）: `pico-SBUS-UART-UF2` 内の `pico_sbus_uart_bridge.uf2`
- Pico 2（RP2350）: `pico2-SBUS-UART-UF2` 内の `pico2_sbus_uart_bridge.uf2`

UF2はBOOTSELを押しながらPicoをUSB接続し、表示される `RPI-RP2` / `RP2350` ドライブへコピーして書き込めます。Arduino Uno R3は `arduino/UnoR3_UART_DShot300/UnoR3_UART_DShot300.ino` をArduino IDEで書き込んでください。ライブラリの追加は不要です。

## 配線

| 送信側 | 受信側 | 備考 |
|---|---|---|
| **既存FC基板のSBUS入力 GP1** | Pico UART0 RX | 従来のフラコンと同じ100000bps, 8E2（SBUS極性も以前のまま） |
| Pico **GP16（PIO UART TX）** | Uno **D0 / RX** | 115200bps, 8N1。3.3Vロジックを受ける入力 |
| Pico GND | Uno GND | 共通GND必須 |
| Uno **D8** | ESC信号線 | 以前のDShot300と同じピン |
| Uno GND | ESC信号GND | 共通GND |
| 1S LiPo | ESC電源 | ESCは別電源で供給 |

**Pico GP17は未使用**です（通信がPico→Unoの一方向なのでRXは必要ありません）。GPIO16/17はともにUART0系のため、SBUS UART0と異なる115200bpsを使うためGPIO16をPIO UART TXとして出力します。既存フライトコントローラの別のGPIO設定は使いません。

UnoのD0はUSBシリアルICにも接続されており、**USBシリアルICのTX出力とPico TXが競合する場合があります。** Unoへの書き込み中は必ずPico→D0の信号線を外してください。運転時にUART波形が乱れる場合はUSBシリアルICを確実に切り離す配線・バッファ構成を使ってください。信号電圧とGNDをオシロで確認するまではプロペラを装着しないでください。UnoのD1/TXをPicoのGPIOへ戻さないでください（5Vの可能性）。

Uno DShot送信コードは元の直接PORTB操作を維持しています。**Uno D9〜D13は使えません。** Uno D7にLED＋抵抗をつなぐと、通信なしは消灯、待機中点滅、使用可能時点灯です。

## SBUS・CHの対応

`pico-copter-fc/firmware/pico_copter/radio.hpp` と `control.cpp` の割り当てを引き継ぎました。

| SBUS | 役割 | この試験 |
|---|---|---|
| CH1 | ラダー/Yaw | 使用しない |
| CH2 | エレベータ/Pitch | 使用しない |
| **CH3** | **スロットル、368〜1680** | **UnoのDShot値へ変換** |
| CH4 | エルロン/Roll | 使用しない |
| CH5 | 従来FCのログ記録 | 使用しない |

CH3が448以下のときは DShot=0（完全停止）、449〜1680を48〜2047へ線形変換、1680以上は2047へ制限します。元の自動10秒ランプは削除しました。

## 安全動作

- SBUSが100ms以上途絶えた場合、または25バイトフレームに`frame-lost`/ `failsafe`フラグがある場合、Picoは直ちに不許可状態をUARTで送信します。
- Pico→UnoのUARTに正しいCRC付きデータが120ms以上届かなくてもUnoはDShot=0を継続送信します。
- Pico・Uno両方とも、受信再開後は**スロットル最下端を200ms維持するまで再始動しません**。
- Unoの電源投入後、最初の3秒間は常にDShot=0を送信します。
- DShot300は500Hzで繰り返し送信します。双方向のeRPM受信はしません。
- 受信機自体がフェイルセーフ時に「正常フラグ付きで最後のCH3値を繰り返す」構成では、受信フレームだけでは送信機喪失を検出できません。受信機の実際のフェイルセーフ設定を確認してください。
- DShot 0はモーターの電源遮断とは異なります。危険時に電源を断てるようにしてください。

## 内部UARTパケット形式

20msごとに8バイト: `A5 5A 01 seq CH3_L CH3_H flags CRC8`

- `flags & 0x01`: SBUS有効（最新フレーム100ms以内、loss/failsafeなし）
- `flags & 0x02`: Pico側の低スロットル再始動条件を満たした
- `CRC8`: 先頭7バイト、初期値0、poly=0x07、MSB-first

## ビルド

Pico SDK 2.2.0、CMake、Ninja、arm-none-eabi-gcc を利用します。

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja -DPICO_BOARD=pico
cmake --build build --parallel
# build/sbus_uart_bridge.uf2
```

`-DPICO_BOARD=pico2` でPico 2向けにもビルドできます。GitHub Actionsが両方のUF2を自動生成・配布します。

プロトコル単体チェック: `python3 tests/check_protocol.py`

### 参照

- [既存のPico FCのSBUS処理](https://github.com/temesotejam/pico-copter-fc/blob/main/firmware/pico_copter/radio.cpp)
- [既存のPico FCの安全処理](https://github.com/temesotejam/pico-copter-fc/blob/main/firmware/pico_copter/sbus_parser.hpp)
- PIO UART TX: Raspberry Pi公式 pico-examples（BSD-3-Clause）
- Uno DShot300: 以前の `UnoR3_DShot300_StartHold_UpDown10s.ino` を基準に信号生成部を維持（原典: bird-sanctuary/arduino-bi-directional-dshot、GPL-3.0）

**未実機検証**: CIでコンパイルとパケット変換の静的試験を行いますが、実基板SBUS/PIO送信、UnoでのDShot波形、およびモーター始動の実機検証は別途必要です。

## 回転・停止を繰り返すときの診断（2026-10-28）

最新のPico UF2はUSBシリアル（115200bpsで開けばよい。USB CDC自体にボーレート指定はありません）に約200ms間隔で、次のような状態を表示します。これはGP16のUART転送とは別なので、PicoからUnoへの制御を変更しません。

```text
PICO ch3=1100 sbus=1 armed=1 age_ms=8 good=1234 fs_events=0 unsafe_tick=0
```

- `ch3` が操作中ずっと一定か確認。
- `sbus=0`、`armed=0` になっていたらPico側で安全停止。
- `fs_events` が増えればSBUSの受信機から一瞬のフェイルセーフ/フレームロスト通知が到着。
- `age_ms` が100ms超ならSBUSの受信が途切れている。
- 新しいUno用スケッチはD6をUART正常/アーム条件（`linkOk`）の信号、D7を**実際に送るDShot指令が非0**の信号として出力。LED（抵抗付き）またはオシロ/ロジックアナライザで確認できます。

**固定スロットルのままモーターだけ止まり、UnoのD7がHIGHのままなら、ESCの回転同期・1S電源の電圧降下・モーター配線が疑わしい**。D7がLOWに変わればUnoがDShot 0を出しているため、PicoログとD6を確認してください。切り分けの途中で始動出力をむやみに上げず、プロペラなしで短時間の試験にしてください。

**重要な配線上の注意**：Uno D0/RXはオンボードUSBシリアル変換器のTXにも接続されています。PCのUSB接続によって電気的に競合する場合があります。UNOをUSB充電器等の電源だけで動作させるなど、PCからD0を駆動しない条件で波形比較してください（Uno本体には安定した5V電源が必要で、1SをUnoの5Vピンに直結してはいけません）。
