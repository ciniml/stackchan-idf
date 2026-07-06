// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Transport-agnostic helpers shared by the two settings pages:
//   - tools/settings.html (BLE)      — loads this as a sibling <script src>
//     (works over file:// for local dev; pages.yml copies it next to the
//     deployed settings.html)
//   - web/settings_wifi.html (HTTP)  — gets this inlined at firmware build
//     time by tools/avatar_dsl/inject.mjs via the SETTINGS_COMMON block
//     comment placeholder (the page must stay a single embeddable file)
//
// Keep this file dependency-free and side-effect-free (pages opt in via
// init()/setupTabs()); both pages assume only `window.StackchanSettings`.
(function () {
  'use strict';

  // --- Board facts -----------------------------------------------------
  // Keyed by the BoardKind byte the firmware reports (BLE BoardKind chr /
  // /api/status "board"). Mirrors board::profile_for() — update BOTH when a
  // board is added. `slug` is the per-board firmware ZIP key in
  // versions.json (release_ota uses the same strings device-side).
  const BOARDS = {
    0: { label: 'CoreS3 + M5 base',                 slug: 'cores3',    servo: true,  battery: true  },
    1: { label: 'CoreS3 + Takao base',              slug: 'cores3',    servo: true,  battery: false },
    2: { label: 'AtomS3R + Atomic ECHO BASE',       slug: 'atoms3r',   servo: false, battery: false },
    3: { label: 'AtomS3 + Atomic ECHO BASE (slim)', slug: 'atoms3',    servo: false, battery: false },
    4: { label: 'M5 StopWatch (C152)',              slug: 'stopwatch', servo: false, battery: true  },
  };

  function boardLabel(kind) {
    const b = BOARDS[kind];
    return b ? b.label : `kind ${kind}`;
  }

  // Firmware-ZIP key for a board kind. Unknown / null (pre-BoardKind
  // firmware) falls back to cores3 — the only board that existed then.
  function boardSlug(kind) {
    const b = BOARDS[kind];
    return b ? b.slug : 'cores3';
  }

  // --- Log panel ---------------------------------------------------------
  // Appends one timestamped line to #log. textContent (not innerHTML) so
  // device-supplied strings can't inject markup. The per-page default css
  // class is set via init() (BLE page historically rendered classless calls
  // as "info" blue; the Wi-Fi page as plain).
  let logDefaultClass = '';
  function log(msg, cls) {
    const el = document.getElementById('log');
    if (!el) return;
    const now = new Date();
    const ts = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}:${String(now.getSeconds()).padStart(2, '0')}`;
    const div = document.createElement('div');
    div.className = 'entry';
    const tsSpan = document.createElement('span');
    tsSpan.className = 'ts';
    tsSpan.textContent = `[${ts}]`;
    const body = document.createElement('span');
    body.className = cls === undefined ? logDefaultClass : cls;
    body.textContent = msg;
    div.appendChild(tsSpan);
    div.appendChild(body);
    el.appendChild(div);
    el.scrollTop = el.scrollHeight;
  }

  // --- Tab bar -----------------------------------------------------------
  // Wires .tabs button[data-tab] to .tab-panel[data-tab] and persists the
  // active tab under `storageKey` (per page, so the BLE and Wi-Fi pages
  // remember their tabs independently).
  function setupTabs(storageKey) {
    const tabs = document.querySelectorAll('.tabs button[data-tab]');
    const panels = document.querySelectorAll('.tab-panel[data-tab]');
    function activate(name) {
      let found = false;
      tabs.forEach(b => {
        const on = b.dataset.tab === name;
        b.classList.toggle('active', on);
        if (on) found = true;
      });
      if (!found) return;
      panels.forEach(p => p.classList.toggle('active', p.dataset.tab === name));
      try { localStorage.setItem(storageKey, name); } catch {}
    }
    tabs.forEach(b => b.addEventListener('click', () => activate(b.dataset.tab)));
    let saved = null;
    try { saved = localStorage.getItem(storageKey); } catch {}
    if (saved) activate(saved);
  }

  function init(opts) {
    opts = opts || {};
    if (opts.logDefaultClass !== undefined) logDefaultClass = opts.logDefaultClass;
    if (opts.tabStorageKey) setupTabs(opts.tabStorageKey);
  }

  // --- Avatar DSL editor ---------------------------------------------------
  // Shared editor logic for #avatar-dsl-section (compile in browser via the
  // injected window.AvatarDsl bundle, presets, downloads, file load). The
  // transport object carries the only genuinely page-specific parts:
  //   isConnected()        -> bool (the Wi-Fi page is always "connected")
  //   send(u8, onProgress) -> Promise. Resolve {ok:true, saved:<bytes
  //                           persisted|null>, note:<msg suffix|''>, raw} or
  //                           {ok:false, error, raw}; reject on transport
  //                           failure. onProgress(sent, total) is optional
  //                           for chunked transports.
  //   reset()              -> Promise<{raw}>; reject on failure.
  //   notConnectedMsg      -> shown when isConnected() is false
  //   compilerMissingMsg   -> shown when window.AvatarDsl is absent
  // Returns {refreshButtons} so pages with a connect/disconnect lifecycle
  // (BLE) can re-gate the buttons, or null when the section isn't in the DOM.
  function setupDslEditor(transport) {
    const $id = (id) => document.getElementById(id);
    const srcEl = $id('dsl-src');
    const msg = $id('dsl-msg');
    const sendBtn = $id('btn-dsl-send');
    const resetBtn = $id('btn-dsl-reset');
    const presetEl = $id('dsl-preset');
    if (!srcEl || !msg) return null;

    const setMsg = (txt, kind = '') => { msg.textContent = txt; msg.className = kind; };

    const compilerOk = !!(window.AvatarDsl && typeof window.AvatarDsl.compile === 'function');
    if (!compilerOk) setMsg(transport.compilerMissingMsg, 'err');

    // Preset selector. Populated only when injection succeeded; otherwise the
    // dropdown stays empty (Array.isArray guard against the raw placeholder
    // string on an un-injected local file:// copy).
    const presets = Array.isArray(window.AVATAR_DSL_PRESETS) ? window.AVATAR_DSL_PRESETS : [];
    const presetMap = {};
    presets.forEach((p, idx) => {
      presetMap[p.name] = p.source;
      const opt = document.createElement('option');
      opt.value = p.name;
      opt.textContent = (idx === 0 ? '↻ ' : '✎ ') + p.name;
      presetEl.appendChild(opt);
    });
    srcEl.value = (presets[0] && presets[0].source) ||
                  (typeof window.AVATAR_DSL_DEFAULT_SOURCE === 'string' &&
                   !window.AVATAR_DSL_DEFAULT_SOURCE.includes('{{AVATAR_DSL_DEFAULT_SOURCE}}')
                     ? window.AVATAR_DSL_DEFAULT_SOURCE : '');
    if (compilerOk) {
      setMsg('準備完了 — Ctrl/Cmd+Enter または「コンパイル & 送信」で反映');
    }

    let lastBuf = null; // last successfully-compiled bytecode (for download)

    function compileNow() {
      if (!compilerOk) {
        setMsg('DSL コンパイラ が読込めていないためコンパイルできません', 'err');
        return null;
      }
      const t0 = performance.now();
      let buf;
      try { buf = window.AvatarDsl.compile(srcEl.value); }
      catch (e) {
        setMsg('コンパイル エラー: ' + e.message, 'err');
        return null;
      }
      const compileMs = performance.now() - t0;
      setMsg(`OK — ${buf.byteLength} bytes, コンパイル ${compileMs.toFixed(1)} ms`, 'ok');
      lastBuf = buf;
      return buf;
    }

    // Shared send tail for compile-and-send and .avbc direct upload. `label`
    // prefixes the result messages ('' for the compile path); onProgress
    // receives (sent, total) from chunked transports.
    async function sendBytes(u8, label, onProgress) {
      try {
        const r = await transport.send(u8, onProgress);
        if (r.ok && r.saved != null) {
          setMsg(`${label}送信完了 — NVS に ${r.saved} バイト保存${r.note || ''}`, 'ok');
          log(`Avatar DSL 送信 OK (${r.saved} B)`, 'ok');
          return true;
        }
        if (r.ok) {
          setMsg(`${label}送信完了 — レスポンス: ` + r.raw, 'ok');
          log('Avatar DSL 送信 OK', 'ok');
          return true;
        }
        setMsg(`${label}送信失敗 — ${r.error || 'unknown'}`, 'err');
        log(`Avatar DSL 送信失敗: ${r.error}`, 'err');
      } catch (e) {
        setMsg('送信エラー: ' + e.message, 'err');
        log('Avatar DSL 送信 失敗: ' + e.message, 'err');
      }
      return false;
    }

    async function compileAndSend() {
      if (!transport.isConnected()) { setMsg(transport.notConnectedMsg, 'err'); return; }
      const buf = compileNow();
      if (!buf) return;
      sendBtn.disabled = true;
      const oldText = sendBtn.textContent;
      sendBtn.textContent = '送信中…';
      try {
        await sendBytes(new Uint8Array(buf), '', (sent, total) => {
          sendBtn.textContent = `送信中… ${sent}/${total} B`;
        });
      } finally {
        sendBtn.textContent = oldText;
        sendBtn.disabled = !transport.isConnected();
      }
    }

    sendBtn.addEventListener('click', compileAndSend);
    // Ctrl/Cmd+Enter inside the textarea triggers compile+send.
    srcEl.addEventListener('keydown', (e) => {
      if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
        e.preventDefault();
        compileAndSend();
      }
    });

    // Preset selector: load source + send. For the first preset (= firmware
    // embedded default) issue a reset instead of re-uploading the same
    // bytecode — leaves the NVS slot empty so future boots use the embedded
    // copy without any persisted override.
    presetEl.addEventListener('change', async () => {
      const name = presetEl.value;
      const src = presetMap[name];
      if (src == null) return;
      srcEl.value = src;
      lastBuf = null;
      if (!transport.isConnected()) {
        setMsg(transport.notConnectedMsg + ' — テキストだけ反映しました', 'err');
        return;
      }
      if (presets[0] && name === presets[0].name) {
        try {
          const r = await transport.reset();
          setMsg(`プリセット「${name}」を読込み — 実機を内蔵デフォルトに復帰` +
                 (r.raw ? ` (${r.raw})` : ''), 'ok');
          log(`Avatar DSL プリセット ${name} (reset)`, 'ok');
        } catch (e) {
          setMsg('リセット失敗: ' + e.message, 'err');
        }
      } else {
        await compileAndSend();
      }
    });

    resetBtn.addEventListener('click', async () => {
      if (!transport.isConnected()) { setMsg(transport.notConnectedMsg, 'err'); return; }
      if (!confirm('実機の顔を firmware 内蔵 デフォルトに戻します (NVS の override を削除)。よろしいですか?')) return;
      const oldText = resetBtn.textContent;
      resetBtn.disabled = true;
      resetBtn.textContent = '送信中…';
      try {
        const r = await transport.reset();
        setMsg(`実機をデフォルト顔に戻しました${r.raw ? ` (${r.raw})` : ''}`, 'ok');
        log('Avatar DSL リセット', 'ok');
      } catch (e) {
        setMsg('リセット失敗: ' + e.message, 'err');
        log('Avatar DSL リセット失敗: ' + e.message, 'err');
      } finally {
        resetBtn.textContent = oldText;
        resetBtn.disabled = !transport.isConnected();
      }
    });

    $id('btn-dsl-download').addEventListener('click', () => {
      let buf = lastBuf;
      if (!buf) buf = compileNow();
      if (!buf) return;
      const blob = new Blob([buf], { type: 'application/octet-stream' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = 'face.avbc';
      document.body.appendChild(a); a.click(); document.body.removeChild(a);
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
    });

    $id('btn-dsl-download-src').addEventListener('click', () => {
      const blob = new Blob([srcEl.value], { type: 'text/plain;charset=utf-8' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = 'face.avdsl';
      document.body.appendChild(a); a.click(); document.body.removeChild(a);
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
    });

    $id('dsl-load').addEventListener('change', (e) => {
      const f = e.target.files[0];
      if (!f) return;
      const r = new FileReader();
      r.onload = () => {
        srcEl.value = r.result;
        setMsg('読込みました — 「コンパイル & 送信」で実機に反映してください');
      };
      r.readAsText(f);
      e.target.value = '';
    });

    // .avbc 直接送信 — for users who already have a compiled bytecode. Skips
    // the in-browser compiler entirely. Only present on pages whose HTML has
    // the file input (currently the BLE page).
    const avbcInput = $id('avbc-file');
    if (avbcInput) {
      avbcInput.addEventListener('change', async (e) => {
        const f = e.target.files[0];
        e.target.value = '';
        if (!f) return;
        if (!transport.isConnected()) { setMsg(transport.notConnectedMsg, 'err'); return; }
        const buf = new Uint8Array(await f.arrayBuffer());
        setMsg(`${f.name} (${buf.length} B) を送信中…`);
        const ok = await sendBytes(buf, `${f.name} `, (sent, total) => {
          setMsg(`${f.name} 送信中… ${sent}/${total} B`);
        });
        if (ok) {
          lastBuf = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
        }
      });
    }

    // Gate send / reset / preset on the transport's connection state. The
    // download / file-load buttons stay enabled (useful pre-connect too).
    function refreshButtons() {
      const connected = transport.isConnected();
      if (sendBtn) sendBtn.disabled = !(connected && compilerOk);
      if (resetBtn) resetBtn.disabled = !connected;
      if (presetEl) presetEl.disabled = !connected;
    }
    refreshButtons();

    return { refreshButtons };
  }

  window.StackchanSettings = { BOARDS, boardLabel, boardSlug, log, setupTabs, init, setupDslEditor };
})();
