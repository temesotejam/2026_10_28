# Pico SBUS → UART → Arduino Uno DShot300

Raspberry Pi Pico / Pico 2 で既存フライトコントローラ基板のSBUSを受信し、GPIO16のPIO UART (115200 8N1) からArduino Uno R3へ転送して、Unoが単方向DShot300でESCを制御するベンチ試験用プロジェクトです。

**プロペラを外して試験してください。** 通信断、SBUSフェイルセーフ、起動時はDShot 0を送信します。ESCへの電源供給を物理的に遮断する機能ではありません。

ソース・配線・ビルド手順はこのリポジトリ内にあります。
