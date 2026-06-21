---
name: release
description: Cut a tagged firmware release for stackchan-idf — push main, tag vX.Y.Z, push the tag, wait for CI, manually trigger Pages (since GITHUB_TOKEN-created releases don't chain-trigger workflows), confirm the live site is serving the new firmware. Use when the user says things like "新しいバージョンをリリース", "vX.Y.Z を出す", "タグを切ってビルド", or simply "release".
---

# stackchan-idf release workflow

End-to-end checklist for publishing a new firmware release. Two workflows are involved:

- `.github/workflows/release.yml` — tag push → ESP-IDF build → GitHub Release
- `.github/workflows/pages.yml` — main / release-publish / dispatch → GitHub Pages deploy with new firmware ZIP

The repo is `ciniml/stackchan-idf` (use `--repo` consistently when invoking `gh`).

## Steps

1. **Sanity check the working tree**
   ```bash
   git status                  # working tree clean except known M5Unified submodule dirt
   git log --oneline -3        # show what's going out
   ```
   Abort if there are unintended changes. The M5Unified submodule will always look dirty locally — that's the `tools/apply-m5-patches.sh` patch, ignore it.

2. **Confirm version number with the user.**
   Look at the latest tag and propose the next bump (patch/minor):
   ```bash
   git tag --list 'v*' | sort -V | tail -3
   ```
   Do not pick the version yourself unless the user has already said it.

3. **(Optional) syntax-check any HTML changes**
   If `tools/settings.html` or `docs/*.html` was touched in this release:
   ```bash
   ./script/check_html_js.sh
   ```

4. **Push main, tag, and push the tag**
   ```bash
   git push origin main
   git tag vX.Y.Z
   git push origin vX.Y.Z
   ```

5. **Watch the release build (~5 min)**
   ```bash
   sleep 15   # let the run start so it shows up in the list
   gh run list --repo ciniml/stackchan-idf --limit 3
   gh run watch <release-run-id> --repo ciniml/stackchan-idf --exit-status
   ```

6. **Verify clean version stamp**
   ```bash
   gh run view <release-run-id> --repo ciniml/stackchan-idf --log | grep "App.*version"
   ```
   Must show `App "stackchan_idf" version: vX.Y.Z` (no `-dirty`). If `-dirty` appears, `version.txt` pin step is broken — investigate.

7. **Confirm the Release exists**
   `gh release view vX.Y.Z --repo ciniml/stackchan-idf` should show the ZIP attached.

8. **Manually trigger pages.yml**
   GitHub's loop-prevention means the release-published event from `GITHUB_TOKEN`-created releases does not chain-trigger downstream workflows. Manual dispatch is required for every release.

   **Pass `--ref <SHA>` explicitly** so the dispatch is pinned to the local
   HEAD even if a parallel `push: main` run hasn't fully indexed yet on
   GitHub's side. A naked `gh workflow run pages.yml` (no `--ref`) reads
   whatever main currently points to, which can be an older commit if push
   indexing is still in flight — and because pages.yml has
   `concurrency.cancel-in-progress: false`, the older-SHA dispatch will
   then finish AFTER the push-triggered run and overwrite it with stale
   content. Has bitten us before (see commit log around v0.6.0).
   ```bash
   LOCAL_SHA=$(git rev-parse HEAD)
   # Belt-and-braces: wait until remote main reports our SHA before
   # dispatching. Usually instant; the loop bails after 30 s if GitHub
   # is having a bad day.
   for _ in $(seq 1 30); do
     REMOTE_SHA=$(gh api repos/ciniml/stackchan-idf/commits/main --jq '.sha')
     [ "$LOCAL_SHA" = "$REMOTE_SHA" ] && break
     sleep 1
   done
   gh workflow run pages.yml --repo ciniml/stackchan-idf --ref "$LOCAL_SHA"
   ```

9. **Watch the pages deploy (~30 s)**
   ```bash
   sleep 5
   gh run list --repo ciniml/stackchan-idf --workflow pages.yml --limit 1
   gh run watch <pages-run-id> --repo ciniml/stackchan-idf --exit-status
   ```
   In the log, look for `[ok] vX.Y.Z -> _site/firmware/vX.Y.Z/firmware-vX.Y.Z.zip`.

10. **Live smoke-test**
    ```bash
    curl -sL https://ciniml.github.io/stackchan-idf/versions.json
    ```
    Expect the new tag at the top of the JSON array. The 301 redirect to `www.fugafuga.org/stackchan-idf/` is expected (custom domain CNAME); follow with `curl -L`. The ZIP itself:
    ```bash
    curl -sL https://ciniml.github.io/stackchan-idf/firmware/vX.Y.Z/firmware-vX.Y.Z.zip -o /dev/null -w "size: %{size_download}\n"
    ```
    Should be ~2.5 MB.

11. **Report to user**
    Summarise:
    - Tag pushed, release run id, pages run id
    - Version stamp from log (must be clean)
    - Live versions.json contents
    - ZIP size

## Known gotchas (already fixed in code, don't regress)

- **403 on softprops/action-gh-release** — release.yml has `permissions: contents: write` at workflow level.
- **404 on actions/configure-pages first run** — pages.yml has `enablement: true`.
- **`-dirty` version suffix** — `tools/apply-m5-patches.sh` dirties the M5Unified submodule worktree which propagates to the parent's `git describe --dirty`. release.yml writes `version.txt` after the patch step; ESP-IDF prefers that over `git describe`.
- **ESP-IDF v6 dropped built-in `json` (cJSON)** — release.yml pinned to `release-v5.4`.
- **Web Bluetooth 512-byte attribute-value cap** — Chrome rejects `writeValueWithResponse` payloads > 512 B. AES-GCM wraps each chunk in 12 B nonce + 16 B tag, so plaintext is bounded by 484 B. `OTA_CHUNK_SIZE` in tools/settings.html is 480.
- **pages.yml path filter** — must include `tools/settings.html` in addition to `docs/**`, otherwise edits to the canonical settings page don't auto-deploy.

## Hardware regression test (manual, not in the recipe)

After the release lands, ideally:

1. Open <https://ciniml.github.io/stackchan-idf/> in Chrome/Edge, pick the new tag, USB-flash a CoreS3.
2. Then open <https://ciniml.github.io/stackchan-idf/settings.html>, BLE-connect, confirm the DIS Firmware Revision reads `vX.Y.Z`.

Not automatable from this skill — flag to the user that this manual step is recommended.
