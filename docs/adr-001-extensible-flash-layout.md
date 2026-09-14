# ADR-001: 拡張パーティションテーブルによるファームウェア・データ配置

- 状態: Accepted（2026-09-15 に配置・起動制御・更新手順を確定。残りは「未決事項」参照）
- 決定日: 2026-09-09
- 対象: CoreS3、AtomS3R、StopWatch、AtomS3

## 背景

現行のファームウェアは、標準のESP-IDFパーティションテーブルで2面のOTAアプリ領域を確保している（`partitions.csv`、`partitions_8mb.csv`、`partitions_16mb.csv`）。8MB版では、HMM有効ビルドに限り、専用`voice`パーティションが無い場合のフォールバックとして待機側OTAスロットをHMM音声の保管領域に流用している（`main/hmm_voice.cpp`）。これは更新時にデータが上書きされるうえ、アプリの実サイズ増加やESP-SRモデル、BlueScriptの実行イメージを安全に収容できない。

現在のビルドサイズは、CoreS3が約3.425MiB、AtomS3RとStopWatchが約3.085MiB、AtomS3が約2.589MiBである。4MiBのアプリ領域は現状動作するが、CoreS3では空きが約589KiBしかない。実際に esp-sr を 2.5.x に更新した際に esp-dl が付随して CoreS3 が 4MiB を超え、2.4.x に固定した実績がある（コミット a5064c9）。ESP-SR、TFLite Micro KWS、BlueScriptの統合や依存コンポーネント更新を考えると、4MiBを将来の固定上限にするのは危険である。

現行のOTAは、稼働中のMainから release-fetch / BLE / Wi-Fi 経由で待機側スロットへ書き込み、`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` と `otadata` により起動確認（`esp_ota_mark_app_valid_cancel_rollback`）に失敗すると旧イメージへ自動で戻る。この2面構成が、後述の決定で手放す主要な性質である。

BlueScriptは、通常のファイルではなく、命令としてMMUにマップする`iflash`、読み取り専用データをマップする`dflash`、起動時に再生する`autorun`を必要とする。これらは連続したrawフラッシュ範囲を必要とし、一般的なSPIFFS/LittleFS上のファイルを直接実行領域として使うことはできない。これらの要件はBlueScript側（外部プロジェクト）の実行モデルに由来し、本リポジトリにはまだBlueScript関連コードが無い。統合着手時に、iflash/dflash/autorunのヘッダ形式と配置要件の一次資料を本ADRから参照できるようにすること。

## 決定

標準パーティションテーブルには、ブートローダーが必ず参照できる最小限の固定領域だけを置く。Mainアプリと大容量データの配置は、固定領域内に保存した拡張パーティションテーブルで管理する。

固定領域は次の役割を持つ。

- カスタム2段目ブートローダー
- 標準パーティションテーブル、NVS、`bootctl`（起動先と起動試行カウンタ、A/B）、`phy_init`
- 小規模なRecoveryアプリ（BLE/Wi-Fi更新と復旧のみ）
- 拡張パーティションテーブルのA/Bコピー

拡張テーブルは、標準テーブルで確保した予約範囲内に、次のような論理領域を記述する。

- Mainアプリ
- BlueScript `iflash`、`dflash`、`autorun`
- ESP-SR/TFLiteモデル
- HMM音声、フォント、顔・動作データ
- 必要に応じた汎用ファイル領域

Mainは論理的には常にMainとして扱い、Recoveryは常に起動可能な固定領域に置く。初期実装ではMainの開始位置を固定し、末尾側の容量を拡張テーブルで変更可能にする。任意の移動は、移行処理が確立してから導入する。

ESP-IDFの`esp_partition` APIを利用するデータ領域は、拡張テーブルを読み込んだ後に、内蔵フラッシュの相対位置・サイズ・ラベルをもつ論理パーティションとして登録する（`esp_partition_register_external`）。既存コードが利用する`esp_partition_find_first()`や`esp_partition_mmap()`を活かし、BlueScriptとモデル管理の利用箇所を共通のパーティション管理層へ接続する。

`esp_partition_register_external`は内蔵フラッシュに対して既存パーティションとの重複を拒否する。そのため、拡張テーブルが管理する予約範囲は**標準パーティションテーブルのエントリとして置かない**（標準テーブル上は単なる未使用範囲とし、範囲の境界は拡張テーブルのヘッダと Recovery/Main のビルド時定数で共有する）。同じ理由で、親データ領域そのものは登録せず、管理用の予約範囲として扱い、子領域だけを登録する。Recovery が Main イメージを書き込む際の`esp_ota_begin`系APIは、外部登録したAPP型（`ota_0`）パーティションに対してそのまま使えることを実機で確認した（2026-09-15）。

標準テーブルは 2 種類用意する（[partitions_adr_16mb.csv](../partitions_adr_16mb.csv) / [partitions_main_16mb.csv](../partitions_main_16mb.csv)、8MB も同様）。機体の正本は前者で、`recovery`（app/factory）、`bootctl`、`exttab` を持ち、予約範囲のエントリは持たない。後者は Main の開発ビルド専用で、IDF のツールチェーンが「最小の app 領域」（`check_sizes`）と「先頭の起動候補」（`idf.py flash` の app オフセット）を使う都合から、`recovery` を載せず `main` を `ota_0` として拡張テーブルと同じ位置・サイズで載せる。この差を吸収するため、ブートローダーと `flash_layout` は `recovery` がテーブルに無ければ `format.h` の固定値（0x10000、1.5 MiB）を使い、Recovery は起動時に自分の領域を app/factory として登録する（IDF はフラッシュ書き込みのたびに実行中 app のパーティションを要求する）。`flash_layout::init()` は標準テーブルの `main` が拡張テーブルの `main` と一致することを検証し、一致すれば登録をスキップする。

カスタムブートローダーは、プロジェクト内の `bootloader_components/main/` で IDF の `bootloader_start.c` を差し替えて実装する（IDF の hooks だけでは起動先を変えられない）。IDF の `bootloader_utility`（イメージ検証・ロード）はそのまま呼び、追加するのは「`bootctl` と拡張テーブルを検証して Main の位置を `esp_partition_pos_t` として渡す」処理だけにする。標準テーブルの読み込み後、拡張テーブルを検証してMainの位置とサイズを取得し、既存のアプリイメージ検証・起動処理へ渡す。拡張テーブルが無効または未作成の場合は、必ずRecoveryを起動する。ブートローダーにはファイルシステムやBlueScript実行環境を組み込まず、固定形式の小さなテーブル検証だけを実装する。

OTAはRecoveryを更新経路として使う。RecoveryがBLE/Wi-FiでMainイメージを受信・検証し、拡張テーブルの対象範囲へ書き込んでから、次回起動先をMainに設定する。Mainの起動確認に失敗した場合はRecoveryへ戻る。Mainの更新中に旧Mainを保持しないため、電断時の復旧責任はRecoveryが担う。Recovery自身の更新は初期リリースでは対象外とする。Recoveryは更新できない前提なので、Recoveryの不具合は出荷後に恒久化する。これがRecoveryの機能を更新・復旧に限定し、表示や音声などの機能を持ち込まない理由である。

### 起動先選択とロールバック

Mainを拡張テーブルで管理するには、現行の`otadata`と`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`による「pending verify → mark valid」の仕組みにも適応が必要となる。本方針では独自の`bootctl`を起動制御の唯一の正本とし、**`otadata`は廃止する**（Recovery は `factory` サブタイプに置き、標準ブートローダーの otadata ロジックは通さない）。`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` は Main / Recovery とも無効にする。標準OTA APIを受信・検証に再利用する場合は、起動先・起動確認状態を更新する処理を分離し、独自制御と競合させない。具体的には、`esp_ota_begin` / `esp_ota_write` / `esp_ota_end` は`otadata`に触れないため再利用対象とし、`otadata`を書き換える`esp_ota_set_boot_partition`および`esp_ota_mark_app_valid_cancel_rollback`は呼ばない。同等の復旧機構を次のように用意する。

- 起動先（Main / Recovery）と起動試行カウンタを、拡張テーブルとは別の専用セクタ（`bootctl`、A/B 2セクタ）に保持する。拡張テーブルと分けるのは、起動のたびに書き換わる値と滅多に変わらない配置情報の消去回数を分離するためである。
- ブートローダーは`bootctl`を読み、起動先がMainかつカウンタが上限未満ならカウンタを進めてMainを起動し、上限に達していればRecoveryを起動する。保存形式は ESP-IDF 5.5.4 の `otadata`（`write_otadata` / `rewrite_ota_seq`）と同じ**消去後書き込み + A/B 世代番号**とする。各セクタは `{magic, format_version, seq(u32), target(u8: 0=Recovery, 1=Main), attempts(u8), max_attempts(u8), pending(u8), request_tag[32], crc32}` を持ち、CRC が正しく seq が大きい側を有効とする。書き込みは常に「無効な側または古い側を消去して新しい seq で書く」ので、途中で電断しても古い側が残る。カウンタは `pending=1` の間しか進まず、Main が起動確認すると `pending=0, attempts=0` で 1 回書く。確認済みの通常起動ではブートローダーは何も書かないため、消去は更新 1 回あたり数回で済む。Flash Encryption とも両立する。
- Mainは起動確認（現行の`esp_ota_mark_app_valid_cancel_rollback`相当）でカウンタを0に戻す。Main側の呼び出し箇所（`main/app_main.cpp`）と、Mainから更新を開始する際に起動先をRecoveryへ切り替える処理は、いずれも共通のパーティション管理層のAPIに置き換える。
- 起動確認後にMainが恒常的にクラッシュする場合、カウンタは毎回0に戻るためRecoveryへは落ちない。これは現行のIDFロールバックと同じ性質であり退行ではないが、起動確認は主要サブシステム（表示、サーボ、無線、音声）の初期化完了後に行い、確認までの猶予を意図的に取る。
- Recoveryは更新完了時に起動先をMainに設定し、カウンタを0にする。
- `bootctl`のA/Bコピーが両方無効、未初期化、または形式非対応の場合はRecoveryを起動する。起動試行状態を安全に永続化できない場合もMainを起動しない。

### 更新開始と電断復旧

Main から更新を始めるときは、Main が `bootctl` に「起動先=Recovery、`request_tag`=取得するリリース タグ」を書いて再起動する。Recovery は起動時に `request_tag` があれば自動で release-fetch を開始し、並行して BLE / HTTP の手動受付も開く。イメージの受信を開始する前に失敗した場合（Wi-Fi 未接続、HTTP エラー、タグ不明）は旧 Main がまだ無傷なので、`request_tag` を消して起動先を Main に戻して再起動する。受信を開始した後の失敗は Recovery に留まる。

Mainの消去や配置変更を始める前に、次回起動先をRecoveryとして`bootctl`へ永続化し、読み戻して確認する。この確認に失敗した場合はMainや既存データの書き換えを開始しない。更新中に再起動してもRecoveryに留まり、転送の再試行または移行処理の復旧を行う。

Mainへの起動切り替えは、Mainイメージの完全受信・検証と、対応する拡張テーブルの検証・確定がすべて完了した後に行う。配置変更を伴う場合は、起動対象イメージとテーブル世代の対応も確認する。拡張テーブルのA/B化だけでは移動中のデータは保護されないため、データの退避・再取得と移行状態の保存方法は別途設計する。

### 現行方式とのトレードオフ

現行の2面OTAでは、稼働中のMainから更新でき、失敗しても旧Mainへ自動で戻る。本決定では次の退行を受け入れる。

- 更新のたびにRecoveryへ再起動して受信するため、更新中はMainの機能（顔・音声・リモコン受信）が止まる。
- 更新に失敗した場合、旧Mainは残らずRecoveryのみが起動する。ユーザーは再度更新を行う必要がある。
- 更新イメージの受信と検証をRecoveryが担うため、Recoveryに Wi-Fi、BLE、TLS（GitHubリリース取得）を含める必要があり、Recoveryのサイズ制約が厳しくなる。

これらは、データ領域を圧迫せずにMainの上限を引き上げるための対価として受け入れる。

## 互換性と整合性

拡張テーブルは 1 セクタ（4 KiB）に収め、ヘッダ `{magic "SCXT", format_version(u16), entry_count(u16), generation(u32), reserved_offset(u32), reserved_size(u32), crc32}` と、エントリ `{offset(u32, 予約範囲先頭からの相対), size(u32), label[16], kind(u8: app / raw / spiffs / littlefs), format_version(u8), flags(u16)}` の配列からなる。読み込み時に、範囲外、重複、アラインメント違反（app は 64 KiB、それ以外は 4 KiB）、未知の必須形式を拒否する。同じ形式定義（ヘッダのみの C）をブートローダーと `components/flash_layout` で共有する。

Main と Recovery は共通コンポーネント `components/flash_layout` を通じて拡張テーブルと `bootctl` を扱う。このコンポーネントは、拡張テーブルの読み込みと子領域の `esp_partition_register_external` 登録、起動確認、更新要求（`request_tag` の書き込みと Recovery への再起動）を提供し、`esp_ota_get_next_update_partition()` や `esp_ota_set_boot_partition()` には依存しない。

拡張テーブルはA/Bコピーを別セクタに保存し、完全に書き込み・検証できた新しいコピーだけを有効にする。両方が無効ならRecoveryへ移行する。

BlueScriptのautorunヘッダーには、現行のRAMアドレスとCRCに加えて、ファームウェア識別子、iflash/dflashの配置世代、領域形式バージョンを記録する。Main更新後に互換性がないプログラムは自動実行せず、再コンパイルまたは再配置を要求する。BlueScript実行中のiflash/dflashの消去・移動は禁止する。

## 容量方針（2026-09-15 確定）

16MB機はRecovery 1.5MiB、Main 5MiB、8MB機はRecovery 1.5MiB、Main 4MiBとする。CoreS3の現行3.425MiBに対して約1.575MiBの増加余地があり、統合後の実サイズが5MiBを超えた時点で6MiBを再評価する。

Recovery 1.5MiBは、最小構成（NimBLE peripheral、Wi-Fi STA、esp_http_server、esp_http_client + mbedTLS、Main と同じ `ota.cpp` / `crypto.cpp` / `release_ota.cpp` を流用、`-Os`、PSRAM なし）を `recovery/` にビルドした実測 0xed350 = 971,600 B（約 0.93 MiB）に基づく（[recovery/README.md](../recovery/README.md)）。拡張テーブル操作、`bootctl`、データ移行の状態機械は数十 KiB 規模であり、枠内に収まる。BLE/Wi-Fi の両経路を維持する。SoftAP プロビジョニングは含めない（必要になれば約 30 KiB）。

データ領域の必要量は次の実測に基づく。HMM 音声（`.htsvoice`）は 1 ファイルずつ保持し、現行のファイルは 0.86〜2.15 MB（mei 0.86 MB、nitech 1.17 MB、tohoku-f01 1.64〜2.15 MB）。esp-sr のモデルは現行 `model` 領域 2.875 MiB。SanoTTS-jp の int8 重みは 559 KB（推論アリーナ 176 KB は RAM）。BlueScript は iflash/dflash/autorun 合計 1 MiB を予約する。

### 16MB機の配置

固定領域（標準パーティションテーブルのエントリ）:

| 名前 | 種別 | オフセット | サイズ | 備考 |
|---|---|---|---|---|
| bootloader | — | 0x0 | 0x8000 | カスタム 2 段目ブートローダー |
| partition table | — | 0x8000 | 0x1000 | 標準テーブル |
| nvs | data/nvs | 0x9000 | 0x4000 | 現行と同じ |
| bootctl | data/0x40 | 0xd000 | 0x2000 | A/B 各 1 セクタ。旧 `otadata` の位置 |
| phy_init | data/phy | 0xf000 | 0x1000 | 現行と同じ |
| recovery | app/factory | 0x10000 | 0x180000 | 1.5 MiB |
| exttab | data/0x41 | 0x190000 | 0x2000 | 拡張テーブル A/B 各 1 セクタ |

予約範囲（標準テーブルには置かない）: 0x1A0000〜0x1000000（14.375 MiB）。拡張テーブルの初期内容:

| ラベル | 用途 | 相対オフセット | サイズ | 絶対オフセット |
|---|---|---|---|---|
| main | app | 0x0 | 0x500000 | 0x1A0000 |
| storage | spiffs | 0x500000 | 0x100000 | 0x6A0000 |
| voice | raw (hmm_voice) | 0x600000 | 0x380000 | 0x7A0000 |
| model | spiffs (esp-sr) | 0x980000 | 0x2E0000 | 0xB20000 |
| sanotts | raw (SanoTTS-jp) | 0xC60000 | 0xA0000 | 0xE00000 |
| bluescript | raw (iflash/dflash/autorun) | 0xD00000 | 0x100000 | 0xEA0000 |
| （未使用） | — | 0xE00000 | 0x60000 | 0xFA0000 |

voice は現行 4 MiB から 3.5 MiB に縮める。1 ファイル運用で最大 2.15 MB なので支障はない。末尾 384 KiB は将来の割り当て用に空けておく。

### 8MB機の配置

固定領域は 16MB 機と同一。予約範囲は 0x1A0000〜0x800000（6.375 MiB）。

| ラベル | 用途 | 相対オフセット | サイズ | 絶対オフセット |
|---|---|---|---|---|
| main | app | 0x0 | 0x400000 | 0x1A0000 |
| storage | spiffs | 0x400000 | 0x80000 | 0x5A0000 |
| sanotts | raw | 0x480000 | 0xA0000 | 0x620000 |
| voice | raw (hmm_voice) | 0x520000 | 0x140000 | 0x6C0000 |

8MB 機は Main 4 MiB を確保すると残りが 2.375 MiB しかないため、storage を 512 KiB に縮める（顔バイトコードと動作データは数十 KiB 規模）。voice は 1.25 MiB で、mei（0.86 MB）と nitech（1.17 MB）は入るが tohoku-f01 は入らない。esp-sr モデルと BlueScript 領域は 8MB 機には置かない（必要になった場合は voice を SanoTTS へ置き換えるなど、ボードごとに判断する）。

## 代替案と不採用理由

- 2つの同サイズOTAアプリを維持する案は、データ領域を圧迫し、現行の待機側スロット流用を温存するため不採用とする。
- Mainを4MiBに固定し続ける案は、CoreS3の余裕が小さく、依存更新で超過した実績もあるため不採用とする。
- データ全体をSPIFFS/LittleFS一つにする案は、BlueScriptの命令mmapに必要な連続・実行可能領域を表現しにくいため不採用とする。ただし、一般ファイルの保存先としては拡張テーブル内の一領域にファイルシステムを置ける。
- アプリ起動後だけで拡張テーブルを登録する案は、ブートローダーがMainの位置を知らず起動できないため不採用とする。

## 移行計画

初回導入はUSB書き込みで、ブートローダー、標準パーティションテーブル、Recovery、Main、拡張テーブルを同時に書き込む。既存のHMM音声、顔データ、動作データ、BlueScript autorunは、必要に応じて退避または再配置する。以後の配置変更はRecovery上で行い、データ移動、拡張テーブルA/B更新、再起動を一つの状態機械として実施する。

## 実装と検証の記録

- Step 1（4a47968）: `recovery/` の最小構成、約 0.93 MiB。
- Step 2（14d14b3）: `components/flash_layout`（形式・検証・アプリ API・ホストテスト）、`tools/flash_layout/gen_exttab.py`、`exttab_16mb.json` / `exttab_8mb.json`。
- Step 3: `bootloader_components/main`（カスタムブートローダー）、`partitions_adr_*.csv` / `partitions_main_*.csv`、Recovery と Main（cores3）の統合。2026-09-15 に CoreS3 で次を確認した。
  - ブートローダーが bootctl を読み Recovery / Main を選択し、拡張テーブルの `main`（0x1A0000）から起動する。
  - Recovery が release-fetch で外部登録した `main` に書き込み、`arm_main`（target=Main, pending=1）で再起動する。起動確認を返さない旧イメージ（v0.12.0）は試行 3 回で Recovery に戻った。
  - 新 Main は起動直後に拡張テーブルの 6 領域を登録し、`ready` 後に `confirm_boot` で pending を落とす。
  - Main からの release-fetch 要求は bootctl にタグを書いて Recovery へ引き継ぎ、Recovery が自動取得する。存在しないタグ（HTTP 404）では受信前に失敗し、`return_to_main` で旧 Main に戻った。
  - BLE OTA: `tools/ble-cli ota` で Main に begin → 「rebooting to recovery」で引き継ぎ → Recovery に再接続して 3.6 MB を 217 秒（16 KiB/s）で受信 → `arm_main` → 試行 1/3 で Main 起動 → 起動確認。
  - 標準テーブルに `recovery` が無い開発用テーブルで Recovery を動かすと、Wi-Fi ドライバの NVS 書き込みで `esp_ota_get_running_partition()` が abort した。Recovery が自分の領域を登録する対処を入れて解消。

## 未決事項

- `bootctl`の`max_attempts`の値と、Mainが起動確認を行うタイミング（どのサブシステム初期化完了を条件とするか）
- ブートローダーの追加サイズ（現行 0x5160、上限 0x8000）
- 更新・配置変更中の電断に対応する移行状態の保存と、イメージ・テーブル世代の対応付け
- リリース パイプライン（release.yml / pages.yml / Web flasher）で bootloader、標準テーブル、Recovery、exttab、bootctl を配布する形（現在は Main の bin のみ）
- cores3 以外のボード（atoms3r / atoms3 / stopwatch）の sdkconfig を新テーブルへ切り替える時期（実機確認後）
- Main の BLE / HTTP アップロード OTA は Recovery への引き継ぎ（再起動）になった。tools/ble-cli には `ota` サブコマンドを追加し、Main に対して実行すると引き継ぎ、再実行で Recovery に送る形で確認済み（3.6 MB を 217 秒、16 KiB/s）。設定ページ（Web Bluetooth）側は `tools/settings.html` に同じ手順（begin → 「rebooting to recovery」→ 同じ BluetoothDevice に `gatt.connect()` を再試行 → 鍵交換をやり直して begin）を実装済み。ブラウザでの実機確認は未実施
- 8MB機でのBlueScript / esp-srモデルの扱い（現時点では置かない）
- BlueScriptのiflash/dflash/autorunヘッダ形式の一次資料への参照
- SanoTTS-jp のモデル配置形式（raw + ヘッダか、ファイルシステム上のファイルか）
- Flash Encryption/Secure Boot使用時の拡張領域の扱い
- Recovery自身を将来更新可能にする方式
