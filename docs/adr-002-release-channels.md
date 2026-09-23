<!--
SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
SPDX-License-Identifier: BSL-1.0
-->

# ADR-002: リリース チャンネル (alpha / beta / rc / stable)

2026-09-24 決定・実装。「アルファ リリースと正式リリースを分け、将来ベータ / RC など段階を
増やせるようにする」。決定事項: alpha は CoreS3 / AtomS3R のみ、`versions.json` は stable 限定、
Pages のプレリリース保持は「進行中のもののみ」、段階名は alpha / beta / rc。
実装の正本は `script/release_channels.py` (CI) と `tools/settings_common.js` (ページ)。

## 1. 現状の配布経路 (変更対象の棚卸し)

| 経路 | ソース | tag の扱い |
|---|---|---|
| GitHub Release 作成 | `.github/workflows/release.yml` | `v*` の push で起動。`prerelease: false` 固定。本文は固定テンプレ |
| Pages ミラー + `versions.json` | `.github/workflows/pages.yml` | `gh release list` (draft 除外、**prerelease は除外していない**) を作成日順に並べ、`RETAIN_FLOOR` (v0.7.0) 未満を `sort -V` で落とす。1 リリース ≈ 25 MB、Pages 上限 1 GB |
| Web Flasher | `docs/index.html` `loadReleases()` | `versions.json` を順に `<select>` へ。先頭が既定 |
| BLE 設定ページの OTA | `tools/settings.html` `loadOtaReleases()` | 同上 (`ota-release`) |
| 機体内 Wi-Fi ページの OTA | `components/wifi_config_service/web/settings_wifi.html` `loadReleaseVersions()` | 機体の `/api/release/versions` (= Pages の `versions.json` をプロキシ、32 KiB 上限キャッシュ) を順に並べ、**先頭 (= 最新) を既定選択** |
| 機体 release-fetch | `release_ota.cpp` | `firmware/<tag>/<board>/stackchan_idf.bin` を取得。`tag_looks_safe()` は `[A-Za-z0-9._-]` 32 文字以内 (コメントに既に `vX.Y.Z-rcN` 想定あり) |
| Recovery 自動取得 | `flash_layout` bootctl `request_tag[32]` | tag 文字列をそのまま保持 |
| DIS Firmware Revision / `/api/status` | `esp_app_desc` version = CI の `version.txt` = tag | 表示のみ |
| リリース手順 | `.claude/skills/release/SKILL.md` | 「次の patch/minor を提案 → tag → pages 手動 dispatch → versions.json 確認」 |

つまり **機体側 (C++) は tag を不透明な文字列として扱っており、そのままで
プレリリース tag に対応できる**。変更が要るのは CI 2 本と Web ページ 3 枚、
それにリリース手順。

## 2. 方針

### 2.1 tag 命名 = SemVer 2.0 の pre-release

```
v0.15.0-alpha.1   v0.15.0-alpha.2 …   ← alpha
v0.15.0-beta.1                        ← beta   (将来)
v0.15.0-rc.1                          ← rc     (将来)
v0.15.0                               ← stable (正式)
```

- 正規表現: `^v(\d+)\.(\d+)\.(\d+)(?:-(alpha|beta|rc)\.(\d+))?$`
- 順序: `alpha < beta < rc < stable` (同じ X.Y.Z 内)、数字は数値比較
- 段階の追加 = **チャンネル表 1 箇所** (§2.3) に名前を足すだけ
- `-` を含む tag は機体側 `tag_looks_safe()` を通り、bootctl の 32 文字にも収まる
  (`v0.15.0-alpha.10` で 16 文字)

> 注意: GNU `sort -V` は `v0.15.0 < v0.15.0-alpha.1` と誤順序にする (実測)。
> pages.yml の `RETAIN_FLOOR` 比較には使えないので、版比較は Python に寄せる (§3.2)。

### 2.2 チャンネルは tag から機械的に導出する (別の入力を持たない)

`channel = suffix ?? "stable"`。release.yml は tag に `-` があれば
`prerelease: true` で Release を作る。GitHub の "Latest" バッジは prerelease に
付かないので、**Releases ページの Latest = 最新 stable** が自動で保たれる。

### 2.3 チャンネルの定義は 2 箇所のミラー (機体は関与しない)

```
script/release_channels.py     CHANNELS = ["alpha", "beta", "rc", "stable"]   (CI / Pages 生成)
tools/settings_common.js       CHANNELS = [...同じ順序...]                     (index.html / settings.html / settings_wifi.html)
```

順序が「どこまで見せるか」の包含関係になる: 利用者が `beta` を選ぶと
beta + rc + stable が見える (alpha は見えない)。

### 2.4 `versions.json` は **stable のみ** のまま据え置き、全チャンネルは別ファイル

```
_site/versions.json         ← 従来どおり stable だけ (既存機体 v0.14.x の /api/release/versions が読む)
_site/versions-all.json     ← 全チャンネル + メタデータ (新しいページ / 新しい機体が読む)
```

`versions-all.json` のエントリ:

```json
{ "tag": "v0.15.0-alpha.2", "channel": "alpha", "prerelease": true,
  "published_at": "2026-09-25T03:00:00Z",
  "boards": { "cores3": "firmware-v0.15.0-alpha.2-cores3.zip", … } }
```

**理由**: 既に配布済みの機体の `settings_wifi.html` は `versions.json` の先頭を
「最新」として既定選択する。ここに alpha を混ぜると、旧機体の利用者が意図せず
alpha へ更新する経路ができる。後方互換のため既存ファイルの意味は変えない。

### 2.5 Pages にミラーするプレリリースは「進行中」のものだけ

Pages 容量 (1 GB) を守るため:

- stable: 従来どおり `RETAIN_FLOOR` 以上を全部
- prerelease: **同じ X.Y.Z 以上の stable がまだ無いもの**だけ (例: `v0.15.0` を出したら
  `v0.15.0-alpha.*` / `-beta.*` / `-rc.*` は Pages から消える。GitHub Releases 上には残る)

### 2.6 ページ側 UI: チャンネル選択 + 既定は stable

3 ページ共通の振る舞い (実装は `settings_common.js` のヘルパに集約):

- `<select>` の手前に「チャンネル: 正式 / RC / ベータ / アルファ」を置く (既定 = 正式)
- 選択は `localStorage` に記憶 (`stackchan.releaseChannel`)
- tag 表示は `v0.15.0-alpha.2  [alpha]` のようにバッジ付き
- `settings_wifi.html` は**動作中 firmware の tag が alpha なら既定チャンネルを alpha に上げる**
  (alpha を入れた人が次の alpha を見つけられるように)。stable 動作中なら stable のまま
- Web Flasher は `?channel=alpha` クエリでも選べるようにしておく (テスターへの案内 URL 用)

### 2.7 機体側の変更は最小 (任意)

- `/api/release/versions` は当面 `versions.json` のまま (stable だけ返る)。
  新 firmware では `?all=1` を付けると `versions-all.json` を取りに行く。
  同梱の `settings_wifi.html` が新しければ `?all=1` を使い、ページ側で絞る。
- `esp_app_desc` の version は tag そのまま (`v0.15.0-alpha.2`) で良い。BLE DIS でも見える。
- **チャンネルの判定・比較を機体には持ち込まない** (更新の自動チェック機能が無い今は不要)。

## 3. 実装計画

### 3.1 `release.yml`
- `prerelease: ${{ contains(github.ref_name, '-') }}`
- 本文先頭に prerelease 時のみ「⚠ これはアルファ / ベータ版です。正式版は Latest を参照」を差し込む
  (`steps.version.outputs.channel` を Python 1 行で出す)
- `softprops/action-gh-release` を v2 に上げて `make_latest: ${{ !contains(github.ref_name, '-') }}` を明示
  (GitHub の既定でも prerelease は Latest にならないが、明示しておく)
- tag 名の妥当性チェック step を追加 (§2.1 の正規表現に合わなければ fail — `v0.15.0-alpha1` のような
  typo で Release が出るのを防ぐ)

### 3.2 `pages.yml` の staging を `script/stage_pages_firmware.py` に移す
- 現在の bash + jq ループ (約 80 行) を Python に置き換え。`gh release list --json tagName,isDraft,isPrerelease,publishedAt`
  と `gh release download` は subprocess で呼ぶ
- semver キーで並べ替え → `versions.json` (stable) と `versions-all.json` (全部) を出力
- §2.5 のプレリリース保持ルールを実装
- 単体テスト (`script/test_stage_pages_firmware.py`、`gh` をモック) で並べ替えと保持ルールを固定

### 3.3 Web 3 ページ + `settings_common.js`
- `settings_common.js`: `CHANNELS`、`parseTag(tag) → {channel, core, n}`、`filterByChannel(list, ch)`、
  `channelBadge(tag)`、`loadChannelPref()/saveChannelPref()`
- `docs/index.html` / `tools/settings.html`: `versions-all.json` を取得し、チャンネル `<select>` で絞る。
  取得失敗時は `versions.json` にフォールバック
- `settings_wifi.html`: `/api/release/versions?all=1` (404 / 旧機体なら `?all` 無し) → 同じヘルパで絞る
- `script/check_html_js.sh` は既存のまま通す

### 3.4 `.claude/skills/release/SKILL.md`
- Step 2 の版提案に「alpha を出すか stable を出すか」を追加。次の alpha 番号は
  `git tag --list 'v0.15.0-alpha.*'` から採番
- Step 10 のスモーク テストを `versions-all.json` (alpha) / `versions.json` (stable) で分ける
- 「stable を出したら同 X.Y.Z のプレリリースが Pages から消えるのは正常」と明記

### 3.5 ドキュメント
- `README.md` の OTA 節に「チャンネル」の説明を 3 行
- 本ファイル (ADR-002) が仕様の正本

## 4. 段階的に出す順序 (提案)

1. **CI だけ先に** (§3.1 + §3.2): tag に `-alpha.N` を付けても Release / Pages が壊れないことを
   `v0.15.0-alpha.1` で確認。この時点では既存ページに alpha は現れない (`versions.json` は stable のみ)
   ので、テスターには GitHub Releases の ZIP か `?channel=alpha` 未対応のため直リンク
   (`firmware/v0.15.0-alpha.1/…`) で渡す
2. **ページ 3 枚** (§3.3): チャンネル選択を追加。ここで alpha が Web Flasher / BLE 設定から選べるようになる
3. **機体** (§2.7 `?all=1`): 次の stable に同梱。旧機体は stable しか見えないまま (意図どおり)
4. beta / rc は `CHANNELS` に既に入れておくので、使い始めるときは tag を打つだけ

## 5. 決定済みの論点 (2026-09-24)

- **alpha の対象ボード**: 4 ボード全部ビルドする (現状の matrix のまま) で良いか。CI 時間は変わらない
- **`versions.json` を stable 限定に固定する案 (§2.4)** で良いか。代案は `versions.json` に
  `channel` を足して全部入れる (旧機体の既定選択が alpha になりうるので非推奨)
- **Pages のプレリリース保持ルール (§2.5)** で良いか。代案は「最新 N 件」
- 段階名は `alpha` / `beta` / `rc` の 3 つで良いか (`nightly` / `dev` を足すなら tag ではなく
  `workflow_dispatch` ビルドの成果物になるので、別の仕組みになる)
