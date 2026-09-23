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
    // proximity: LTR-553 on the CoreS3 mainboard (both CoreS3 bases).
    0: { label: 'CoreS3 + M5 base',                 slug: 'cores3',    servo: true,  battery: true,  proximity: true  },
    1: { label: 'CoreS3 + Takao base',              slug: 'cores3',    servo: true,  battery: false, proximity: true  },
    2: { label: 'AtomS3R + Atomic ECHO BASE',       slug: 'atoms3r',   servo: false, battery: false, proximity: false },
    3: { label: 'AtomS3 + Atomic ECHO BASE (slim)', slug: 'atoms3',    servo: false, battery: false, proximity: false },
    4: { label: 'M5 StopWatch (C152)',              slug: 'stopwatch', servo: false, battery: true,  proximity: false },
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

  // --- Servo limits form + range-setting calibration -----------------------
  // Shared logic for the #servo-section form (sv-yaw-*/sv-pitch-* inputs) and
  // the hand-driven range calibration: toggle range mode → device drops
  // torque → live positions poll into #sv-live-pos → capture buttons stash
  // raw/degree values into the form. The transport object carries the
  // page-specific parts:
  //   canToggle()      -> bool; gates the range-mode button (BLE: chr + key)
  //   canCapture()     -> bool; extra gate on the capture buttons
  //   setRangeMode(on) -> Promise; reject on failure
  //   readPositions()  -> Promise<{yaw, pitch}> raw steps, -1 = unknown;
  //                       reject on transient failure (display keeps the
  //                       last reading)
  //   pollMs           -> live-position poll period while range mode is on
  // Returns null when the servo section isn't in the DOM.
  const SERVO_DEFAULTS = { yaw_zero: 460, yaw_min: -40, yaw_max: 40,
                           pitch_zero: 620, pitch_min: -10, pitch_max: 25 };
  function setupServoCalibration(transport) {
    const $id = (id) => document.getElementById(id);
    if (!$id('sv-range-mode') || !$id('sv-yaw-zero')) return null;

    let rangeOn = false;
    let liveYaw = -1, livePitch = -1;
    let pollTimer = null;

    function readForm() {
      const num = (id, def) => {
        const v = parseInt($id(id).value, 10);
        return Number.isFinite(v) ? v : def;
      };
      return {
        yaw_zero: num('sv-yaw-zero', SERVO_DEFAULTS.yaw_zero),
        yaw_min:  num('sv-yaw-min',  SERVO_DEFAULTS.yaw_min),
        yaw_max:  num('sv-yaw-max',  SERVO_DEFAULTS.yaw_max),
        pitch_zero: num('sv-pitch-zero', SERVO_DEFAULTS.pitch_zero),
        pitch_min:  num('sv-pitch-min',  SERVO_DEFAULTS.pitch_min),
        pitch_max:  num('sv-pitch-max',  SERVO_DEFAULTS.pitch_max),
      };
    }
    function formJson() { return JSON.stringify(readForm()); }
    function seedForm(json) {
      let o = { ...SERVO_DEFAULTS };
      if (json) { try { Object.assign(o, JSON.parse(json)); } catch {} }
      $id('sv-yaw-zero').value   = o.yaw_zero;
      $id('sv-yaw-min').value    = o.yaw_min;
      $id('sv-yaw-max').value    = o.yaw_max;
      $id('sv-pitch-zero').value = o.pitch_zero;
      $id('sv-pitch-min').value  = o.pitch_min;
      $id('sv-pitch-max').value  = o.pitch_max;
    }

    function rawDeltaToDeg(raw, zero) {
      return Math.round((raw - zero) * 5 / 16);
    }

    function refreshUi() {
      const ok = rangeOn && transport.canCapture();
      for (const id of ['sv-cap-yaw-zero','sv-cap-yaw-min','sv-cap-yaw-max',
                        'sv-cap-pitch-zero','sv-cap-pitch-min','sv-cap-pitch-max']) {
        const el = $id(id);
        if (el) el.disabled = !ok;
      }
      const btn = $id('sv-range-mode');
      btn.textContent = '範囲設定モード: ' + (rangeOn ? 'ON' : 'OFF');
      btn.disabled = !transport.canToggle();
    }

    function clearLiveDisplay() {
      $id('sv-live-pos').textContent = 'Yaw: -- (--°)   Pitch: -- (--°)';
    }

    async function refreshPositions() {
      try {
        const p = await transport.readPositions();
        liveYaw = p.yaw;
        livePitch = p.pitch;
        const zy = parseInt($id('sv-yaw-zero').value, 10) || SERVO_DEFAULTS.yaw_zero;
        const zp = parseInt($id('sv-pitch-zero').value, 10) || SERVO_DEFAULTS.pitch_zero;
        const fmt = (raw, z) => raw >= 0
          ? `${raw} (${rawDeltaToDeg(raw, z) >= 0 ? '+' : ''}${rawDeltaToDeg(raw, z)}°)`
          : '--';
        $id('sv-live-pos').textContent = `Yaw: ${fmt(liveYaw, zy)}   Pitch: ${fmt(livePitch, zp)}`;
      } catch (e) {
        // Transient — leave the last reading visible.
      }
    }

    function restartPoll() {
      clearInterval(pollTimer); pollTimer = null;
      if (rangeOn) {
        pollTimer = setInterval(refreshPositions, transport.pollMs);
        refreshPositions();
      }
    }

    async function setRangeMode(on) {
      if (!transport.canToggle()) return;
      try {
        await transport.setRangeMode(on);
        rangeOn = on;
        refreshUi();
        restartPoll();
        if (on) {
          log('範囲設定モード: ON (サーボの力が抜けます)', 'ok');
        } else {
          log('範囲設定モード: OFF');
          liveYaw = livePitch = -1;
          clearLiveDisplay();
        }
      } catch (e) {
        log('範囲設定モード切替失敗: ' + e.message, 'err');
      }
    }

    // Range mode toggled from elsewhere (another tab / an earlier session) —
    // adopt the device-reported state without re-writing it.
    function syncRangeOn(on) {
      if (on === rangeOn) return;
      rangeOn = on;
      refreshUi();
      restartPoll();
    }

    // Transport lost (BLE disconnect): stop polling and drop back to the
    // idle display. The device auto-OFFs range mode on its side.
    function reset() {
      rangeOn = false;
      liveYaw = livePitch = -1;
      clearInterval(pollTimer); pollTimer = null;
      clearLiveDisplay();
      refreshUi();
    }

    function capture(axis, which) {
      const live = axis === 'yaw' ? liveYaw : livePitch;
      if (live < 0) { log('まだサーボ位置を取得できていません', 'err'); return; }
      const zeroId = `sv-${axis}-zero`;
      if (which === 'zero') {
        $id(zeroId).value = live;
      } else {
        const z = parseInt($id(zeroId).value, 10) ||
                  SERVO_DEFAULTS[axis === 'yaw' ? 'yaw_zero' : 'pitch_zero'];
        $id(`sv-${axis}-${which}`).value = rawDeltaToDeg(live, z);
      }
      // Re-flow the live °-display (the zero may have just moved).
      refreshPositions();
    }

    $id('sv-range-mode').addEventListener('click', () => setRangeMode(!rangeOn));
    for (const axis of ['yaw', 'pitch']) {
      for (const which of ['zero', 'min', 'max']) {
        const el = $id(`sv-cap-${axis}-${which}`);
        if (el) el.addEventListener('click', () => capture(axis, which));
      }
    }

    return { readForm, formJson, seedForm, refreshUi, refreshPositions,
             setRangeMode, syncRangeOn, reset, isOn: () => rangeOn };
  }

  // --- Secret-field buttons (MCP token / auth password) --------------------
  // Identical on both pages: random-generate / show-toggle / clear for
  // #mcp-token, and the confirm-guarded clear for #auth-password. The clear
  // paths set el.dataset.cleared so each page's Apply logic sends an explicit
  // empty value rather than treating an empty field as "leave as-is".
  const MCP_TOKEN_ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
  function generateMcpToken(len = 32) {
    const out = new Uint8Array(len);
    crypto.getRandomValues(out);
    let s = '';
    for (let i = 0; i < len; ++i) {
      s += MCP_TOKEN_ALPHABET[out[i] % MCP_TOKEN_ALPHABET.length];
    }
    return s;
  }
  function setupSecretFields() {
    const $id = (id) => document.getElementById(id);
    const tokenEl = $id('mcp-token');
    if (tokenEl) {
      $id('btn-mcp-token-gen').addEventListener('click', () => {
        tokenEl.value = generateMcpToken();
        tokenEl.type = 'text';
        delete tokenEl.dataset.cleared;
      });
      $id('btn-mcp-token-show').addEventListener('click', () => {
        tokenEl.type = (tokenEl.type === 'password') ? 'text' : 'password';
      });
      $id('btn-mcp-token-clear').addEventListener('click', () => {
        tokenEl.value = '';
        tokenEl.type = 'password';
        tokenEl.dataset.cleared = '1';
      });
    }
    const authEl = $id('auth-password');
    const authClear = $id('btn-auth-password-clear');
    if (authEl && authClear) {
      authClear.addEventListener('click', () => {
        if (!confirm('認証なし (空欄) に戻します。保存 & 再起動で反映されます。よろしいですか?')) {
          return;
        }
        authEl.value = '';
        authEl.dataset.cleared = '1';
        const hintEl = $id('auth-password-state');
        if (hintEl) hintEl.textContent = '(クリア予約 — 保存で空欄を送信)';
      });
    }
  }

  // --- LT timekeeper config form -------------------------------------------
  // Shared form <-> compact-JSON mapping for the #lt-section inputs
  // (lt-total / lt-warn / lt-soon / lt-repeat numbers + the four
  // text/reading announcement pairs). See main/lt_timer.hpp for the schema.

  // Build the compact JSON from whichever fields are filled. Empty form →
  // null (= nothing to send; firmware keeps current values).
  function buildLtConfigJson() {
    const $id = (id) => document.getElementById(id);
    const num = (id) => { const v = $id(id).value.trim(); return v === '' ? null : Number(v); };
    const str = (id) => { const v = $id(id).value.trim(); return v === '' ? null : v; };
    const o = {};
    const total = num('lt-total');   if (total  != null && total  > 0) o.total_s  = total;
    const warn  = num('lt-warn');    if (warn   != null && warn   > 0) o.warn_s   = warn;
    const soon  = num('lt-soon');    if (soon   != null && soon   >= 0) o.soon_s   = soon;
    const rep   = num('lt-repeat');  if (rep    != null && rep    >= 0) o.repeat_s = rep;
    for (const key of ['warn', 'soon', 'just', 'over']) {
      const t = str(`lt-${key}-text`), r = str(`lt-${key}-reading`);
      if (t || r) { o[key] = {}; if (t) o[key].text = t; if (r) o[key].reading = r; }
    }
    return Object.keys(o).length ? JSON.stringify(o) : null;
  }

  // Populate the form from the device-reported JSON. Missing / unparsable
  // JSON leaves the form untouched (placeholders show the firmware defaults).
  function seedLtConfigForm(json) {
    if (!json) return;
    let o;
    try { o = JSON.parse(json); } catch { return; }
    const $id = (id) => document.getElementById(id);
    if (o.total_s  != null) $id('lt-total').value  = o.total_s;
    if (o.warn_s   != null) $id('lt-warn').value   = o.warn_s;
    if (o.soon_s   != null) $id('lt-soon').value   = o.soon_s;
    if (o.repeat_s != null) $id('lt-repeat').value = o.repeat_s;
    for (const key of ['warn', 'soon', 'just', 'over']) {
      if (!o[key]) continue;
      if (o[key].text) $id(`lt-${key}-text`).value = o[key].text;
      if (o[key].reading) $id(`lt-${key}-reading`).value = o[key].reading;
    }
  }

  // --- jtts phrase list mapping ---------------------------------------------
  // Textarea line format: `表示 | 読み` (reading optional). JSON entry: a
  // plain string when display == reading (compact — the JttsConfig budget is
  // ~768 bytes), otherwise {text, reading}. The device parser accepts both.
  // MUST be used by both pages: a page that renders entries with a bare
  // join('\n') turns object entries into "[object Object]" and then persists
  // the corruption on the next save (bit the Wi-Fi page on HW).
  function jttsPhrasesFromText(text) {
    return text
      .split('\n')
      .map((line) => {
        const bar = line.indexOf('|');
        const t = (bar < 0 ? line : line.slice(0, bar)).trim();
        const r = (bar < 0 ? line : line.slice(bar + 1)).trim();
        return { text: t, reading: r };
      })
      .filter((p) => p.text.length > 0 || p.reading.length > 0)
      .map((p) => {
        const t = p.text || p.reading;
        const r = p.reading || p.text;
        return t === r ? t : { text: t, reading: r };
      });
  }
  function jttsPhrasesToText(arr) {
    if (!Array.isArray(arr)) return '';
    return arr.map((p) => {
      if (typeof p === 'string') return p;
      const t = p.text ?? p.reading ?? '';
      const r = p.reading ?? p.text ?? '';
      return t === r ? t : `${t} | ${r}`;
    }).join('\n');
  }

  // --- Proximity sensor UI (shared by both settings pages) ----------------
  // The section's DOM ids are identical on the BLE and Wi-Fi pages; the
  // transport supplies read/write functions. PS counts: ~20-30 nothing in
  // front, ~40 at 10 cm, ~80 at 5 cm, 1500+ at contact (fixed emitter).
  const PROX_METER_MAX = 800;
  function proxFormValues() {
    const num = (id, max) => {
      const n = Number(document.getElementById(id).value);
      return Number.isFinite(n) ? Math.max(0, Math.min(max, Math.round(n))) : 0;
    };
    let near = num('prox-near', 2047), far = num('prox-far', 2047);
    // Keep near above far (the firmware does the same). Raising far past
    // near lifts near with it so the pair stays a hysteresis band.
    if (far >= near) {
      near = Math.min(2047, far + 1);
      document.getElementById('prox-near').value = near;
    }
    return {
      enabled: document.getElementById('prox-enabled').checked,
      near, far,
      hold_ms: num('prox-hold', 5000), cooldown_s: num('prox-cooldown', 120),
    };
  }
  function proxFillForm(v) {
    if (!v) return;
    if (typeof v.enabled === 'boolean') document.getElementById('prox-enabled').checked = v.enabled;
    if (typeof v.near === 'number') document.getElementById('prox-near').value = v.near;
    if (typeof v.far === 'number') document.getElementById('prox-far').value = v.far;
    if (typeof v.hold_ms === 'number') document.getElementById('prox-hold').value = v.hold_ms;
    if (typeof v.cooldown_s === 'number') document.getElementById('prox-cooldown').value = v.cooldown_s;
  }
  // Live reading → meter + text. `st` = {raw, near_now, available}.
  function proxShowLive(st) {
    const raw = document.getElementById('prox-raw');
    const meter = document.getElementById('prox-meter');
    const state = document.getElementById('prox-state');
    if (!raw || !meter || !state) return;
    if (!st || !st.available) {
      raw.textContent = '—'; meter.value = 0; state.textContent = 'センサーなし';
      return;
    }
    raw.textContent = String(st.raw);
    meter.value = Math.min(PROX_METER_MAX, st.raw);
    state.textContent = (st.near_now ? '近い' : '遠い') + (st.saturated ? ' (飽和)' : '');
    state.className = st.near_now ? 'prox-near' : '';
  }
  // 「今の値を near にする」: near = current reading, far = 60 % of it (but
  // never below the idle floor guess of 30).
  function proxAdoptCurrent() {
    const raw = Number(document.getElementById('prox-raw').textContent);
    if (!Number.isFinite(raw) || raw <= 0) return false;
    const near = Math.max(1, Math.round(raw));
    document.getElementById('prox-near').value = near;
    document.getElementById('prox-far').value = Math.max(Math.min(30, near - 1), Math.round(near * 0.6));
    return true;
  }
  // Live readout. proxLivePolling(fetch) registers the transport; the page
  // calls proxLiveStart() once the device is reachable (BLE connect / Wi-Fi
  // page load) and proxLiveStop() when it isn't. The #prox-live checkbox
  // mirrors the state so the user can pause the traffic. Samples feed a
  // 60-point sparkline so a hand pass is visible even between updates.
  let proxTimer = null;
  let proxFetch = null;
  const proxHistory = [];
  function proxDrawHistory() {
    const cv = document.getElementById('prox-spark');
    if (!cv) return;
    const ctx = cv.getContext('2d');
    const w = cv.width, h = cv.height;
    ctx.clearRect(0, 0, w, h);
    ctx.strokeStyle = '#999'; ctx.lineWidth = 1;
    const nearTh = Number(document.getElementById('prox-near').value) || 0;
    const farTh = Number(document.getElementById('prox-far').value) || 0;
    const yOf = (v) => h - 1 - Math.min(1, v / PROX_METER_MAX) * (h - 2);
    ctx.setLineDash([3, 3]);
    ctx.strokeStyle = '#e66'; ctx.beginPath(); ctx.moveTo(0, yOf(nearTh)); ctx.lineTo(w, yOf(nearTh)); ctx.stroke();
    ctx.strokeStyle = '#6a6'; ctx.beginPath(); ctx.moveTo(0, yOf(farTh)); ctx.lineTo(w, yOf(farTh)); ctx.stroke();
    ctx.setLineDash([]);
    ctx.strokeStyle = '#37c'; ctx.lineWidth = 2; ctx.beginPath();
    proxHistory.forEach((v, i) => {
      const x = (i / 59) * (w - 1);
      if (i === 0) ctx.moveTo(x, yOf(v)); else ctx.lineTo(x, yOf(v));
    });
    ctx.stroke();
  }
  function proxLiveStop() {
    if (proxTimer) { clearInterval(proxTimer); proxTimer = null; }
    const cb = document.getElementById('prox-live');
    if (cb) cb.checked = false;
  }
  function proxLiveStart() {
    if (!proxFetch || !document.getElementById('prox-live')) return;
    proxLiveStop();
    document.getElementById('prox-live').checked = true;
    let busy = false;
    proxTimer = setInterval(async () => {
      if (document.hidden || busy) return;
      busy = true;
      try {
        const st = await proxFetch();
        proxShowLive(st);
        if (st && st.available) {
          proxHistory.push(st.raw);
          if (proxHistory.length > 60) proxHistory.shift();
          proxDrawHistory();
        }
      } catch (e) { /* transport hiccup: keep polling */ }
      busy = false;
    }, 500);
  }
  function proxLivePolling(fetchStatus) {
    proxFetch = fetchStatus;
    const cb = document.getElementById('prox-live');
    if (!cb) return;
    cb.addEventListener('change', () => (cb.checked ? proxLiveStart() : proxLiveStop()));
    document.addEventListener('visibilitychange', () => { if (document.hidden) proxLiveStop(); });
  }
  // The markup, identical on both pages (inserted by each page at load).
  const PROX_SECTION_HTML = `
      <h2 class="section-title" id="prox-section-title">近接センサー (CoreS3)</h2>
      <label class="toggle-label" for="prox-enabled">
        <input type="checkbox" id="prox-enabled">
        <span>手を近づけると反応する</span>
      </label>
      <p class="hint">画面上辺のセンサーに手を近づけると、うれしい顔になって近接フレーズ (音声タブで設定、無ければ吹き出しのみ) を話します。会話中は反応しません。値は反射光量 (0〜2047) で、既定の前段 (ゲイン x64・15 パルス) では何もないとき 200 前後、既定の閾値は near 335 / far 240 です。すべて即時反映。</p>
      <div class="prox-live-box">
        <div class="prox-live-row">
          <span class="prox-live-label">現在値</span>
          <span id="prox-raw">—</span>
          <span id="prox-state"></span>
          <meter id="prox-meter" min="0" max="${PROX_METER_MAX}" value="0"></meter>
          <label class="toggle-label" for="prox-live" style="display:inline-flex;margin:0"><input type="checkbox" id="prox-live"><span>リアルタイム更新</span></label>
        </div>
        <canvas id="prox-spark" width="360" height="60"></canvas>
        <div class="prox-live-row">
          <button type="button" id="prox-adopt" class="secondary">今の値を near にする</button>
          <span class="hint" style="margin:0">赤の点線 = near、緑 = far。接続すると自動で更新が始まります (0.5 秒ごと)。</span>
        </div>
      </div>
      <div class="prox-grid">
        <label for="prox-near">near (以上で近い)</label><input type="number" id="prox-near" min="0" max="2047">
        <label for="prox-far">far (未満で遠い)</label><input type="number" id="prox-far" min="0" max="2047">
        <label for="prox-hold">判定の連続時間 [ms]</label><input type="number" id="prox-hold" min="0" max="5000">
        <label for="prox-cooldown">反応の間隔 [s]</label><input type="number" id="prox-cooldown" min="0" max="120">
      </div>
      <div class="prox-live-row">
        <button type="button" id="prox-apply">閾値を適用 (即時・再起動なし)</button>
        <span id="prox-apply-msg" class="hint" style="margin:0"></span>
      </div>
      <p class="hint">far は near より小さくしてください (近い→遠いのちらつき防止)。反応が敏感すぎるなら near を上げるか連続時間を伸ばします。上の 4 項目は「閾値を適用」を押した時点で機体に反映・保存されます (ページ下部の保存でも送られます)。</p>
      <details id="prox-tuning">
        <summary>センサー調整モード (受光ゲイン / LED / 測定周期 / オフセット)</summary>
        <p class="hint">LTR-553 の前段を変えると生の値のスケールが変わります。「現在値を表示」を有効にしたまま値を変えて「センサーに適用」を押し、望む距離で十分な差が出る組み合わせを探してから near / far を決め直してください。到達距離を伸ばすなら ゲイン↑・電流↑・パルス数↑ の順に効きます (飽和したら「飽和」と出ます)。オフセットは何もない時の値 (クロストーク) を引き算します。</p>
        <div class="prox-grid">
          <label for="prox-gain">受光ゲイン</label>
          <select id="prox-gain"><option value="0">x16</option><option value="1">x32</option><option value="2">x64</option></select>
          <label for="prox-led-current">LED 電流</label>
          <select id="prox-led-current"><option value="0">5 mA</option><option value="1">10 mA</option><option value="2">20 mA</option><option value="3">50 mA</option><option value="4">100 mA</option></select>
          <label for="prox-led-duty">LED duty</label>
          <select id="prox-led-duty"><option value="0">25 %</option><option value="1">50 %</option><option value="2">75 %</option><option value="3">100 %</option></select>
          <label for="prox-led-freq">LED パルス周波数</label>
          <select id="prox-led-freq"><option value="0">30 kHz</option><option value="1">40 kHz</option><option value="2">50 kHz</option><option value="3">60 kHz</option><option value="4">70 kHz</option><option value="5">80 kHz</option><option value="6">90 kHz</option><option value="7">100 kHz</option></select>
          <label for="prox-pulses">パルス数 (1〜15)</label><input type="number" id="prox-pulses" min="1" max="15">
          <label for="prox-meas-rate">測定周期</label>
          <select id="prox-meas-rate"><option value="7">10 ms</option><option value="0">50 ms</option><option value="1">70 ms</option><option value="2">100 ms</option><option value="3">200 ms</option><option value="4">500 ms</option><option value="5">1000 ms</option><option value="6">2000 ms</option></select>
          <label for="prox-offset">オフセット (0〜1023)</label><input type="number" id="prox-offset" min="0" max="1023">
        </div>
        <div class="prox-live-row">
          <button type="button" id="prox-tune-apply">センサーに適用</button>
          <button type="button" id="prox-tune-offset" class="secondary">今の値をオフセットにする</button>
          <button type="button" id="prox-tune-default" class="secondary">既定値に戻す</button>
          <span id="prox-tune-msg" class="hint"></span>
        </div>
        <p class="hint">オフセット: 何もかざしていない状態で「今の値をオフセットにする」→「センサーに適用」すると、床の値 (クロストーク) をチップ内で引き算して 0 付近にできます。ゲインを上げたときは先にこれをやると near / far を小さいままにできます (オフセットは機体ごとに違うので既定値にはしません)。</p>
      </details>`;
  function proxInsertSection(afterEl) {
    if (!afterEl || document.getElementById('prox-section-title')) return;
    if (!document.getElementById('prox-style')) {
      const st = document.createElement('style');
      st.id = 'prox-style';
      st.textContent = `
        .prox-live-box { border:1px solid #ccc; border-radius:6px; padding:0.5rem 0.7rem; margin:0.4rem 0; background:#fafafa; }
        .prox-live-row { display:flex; align-items:center; gap:0.6rem; flex-wrap:wrap; margin:0.3rem 0; }
        .prox-live-row meter { width:10rem; height:1rem; }
        .prox-live-label { color:#666; }
        #prox-raw { font-family:monospace; font-size:1.6rem; font-weight:bold; min-width:3.5em; text-align:right; }
        #prox-spark { display:block; width:100%; max-width:360px; height:60px; background:#fff; border:1px solid #ddd; }
        #prox-state.prox-near { color:#0a7; font-weight:bold; }
        .prox-grid { display:grid; grid-template-columns:auto 7rem; gap:0.3rem 0.8rem; align-items:center; max-width:24rem; margin:0.4rem 0; }
        .prox-grid input { width:100%; box-sizing:border-box; }`;
      document.head.appendChild(st);
    }
    const wrap = document.createElement('div');
    wrap.id = 'prox-section';
    wrap.innerHTML = PROX_SECTION_HTML;
    afterEl.insertAdjacentElement('afterend', wrap);
    const adopt = document.getElementById('prox-adopt');
    if (adopt) adopt.addEventListener('click', () => { if (!proxAdoptCurrent()) alert('現在値が無いので「現在値を表示」を先に有効にしてください'); });
  }
  const PROX_TUNING_DEFAULT = { gain: 2, led_freq: 3, led_duty: 3, led_current: 4, pulses: 15, meas_rate: 0, offset: 0 };
  function proxTuningValues() {
    const num = (id, max, min = 0) => {
      const n = Number(document.getElementById(id).value);
      return Number.isFinite(n) ? Math.max(min, Math.min(max, Math.round(n))) : min;
    };
    return {
      gain: num('prox-gain', 2), led_freq: num('prox-led-freq', 7), led_duty: num('prox-led-duty', 3),
      led_current: num('prox-led-current', 4), pulses: num('prox-pulses', 15, 1),
      meas_rate: num('prox-meas-rate', 7), offset: num('prox-offset', 1023),
    };
  }
  function proxFillTuning(v) {
    if (!v) return;
    for (const [k, id] of [['gain','prox-gain'],['led_freq','prox-led-freq'],['led_duty','prox-led-duty'],
                           ['led_current','prox-led-current'],['pulses','prox-pulses'],
                           ['meas_rate','prox-meas-rate'],['offset','prox-offset']]) {
      if (typeof v[k] === 'number') document.getElementById(id).value = String(v[k]);
    }
  }
  // Thresholds "閾値を適用" button. apply(values) → Promise (page transport).
  function proxSetupApply(apply) {
    const btn = document.getElementById('prox-apply');
    const msg = document.getElementById('prox-apply-msg');
    if (!btn) return;
    btn.addEventListener('click', async () => {
      msg.textContent = '適用中…';
      try {
        const v = proxFormValues();
        await apply(v);
        msg.textContent = `適用しました (near=${v.near} far=${v.far} hold=${v.hold_ms}ms cooldown=${v.cooldown_s}s)`;
        proxDrawHistory();
      } catch (e) { msg.textContent = '失敗: ' + e.message; }
    });
  }
  // apply(values) → Promise; the page supplies the transport.
  function proxSetupTuning(apply) {
    const msg = document.getElementById('prox-tune-msg');
    const btn = document.getElementById('prox-tune-apply');
    const dflt = document.getElementById('prox-tune-default');
    if (!btn) return;
    btn.addEventListener('click', async () => {
      msg.textContent = '適用中…';
      try { await apply(proxTuningValues()); msg.textContent = '適用しました — near / far を決め直してください'; }
      catch (e) { msg.textContent = '失敗: ' + e.message; }
    });
    dflt.addEventListener('click', () => { proxFillTuning(PROX_TUNING_DEFAULT); msg.textContent = '既定値を入れました (未適用)'; });
    const offBtn = document.getElementById('prox-tune-offset');
    if (offBtn) offBtn.addEventListener('click', () => {
      // The chip subtracts the offset from the raw count, so the reading
      // shown already has the current offset removed: new = old + reading.
      const raw = Number(document.getElementById('prox-raw').textContent);
      if (!Number.isFinite(raw)) { msg.textContent = '現在値がありません (接続してリアルタイム更新を待ってください)'; return; }
      const cur = Number(document.getElementById('prox-offset').value) || 0;
      document.getElementById('prox-offset').value = Math.min(1023, cur + raw);
      msg.textContent = `オフセット ${Math.min(1023, cur + raw)} を入れました — 「センサーに適用」で反映`;
    });
  }

  function proxSetVisible(visible) {
    const el = document.getElementById('prox-section');
    if (el) el.style.display = visible ? '' : 'none';
    if (!visible && proxTimer) { clearInterval(proxTimer); proxTimer = null; }
  }

  // --- Release channels --------------------------------------------------
  // Mirror of script/release_channels.py (CI side). Tags follow SemVer
  // pre-release grammar: vX.Y.Z[-(alpha|beta|rc).N]. CHANNELS is ordered
  // from least to most stable; picking channel C shows C and everything
  // after it. Add a stage in BOTH files.
  const CHANNELS = ['alpha', 'beta', 'rc', 'stable'];
  const CHANNEL_LABELS = { alpha: 'アルファ', beta: 'ベータ', rc: 'RC', stable: '正式' };
  const CHANNEL_PREF_KEY = 'stackchan.releaseChannel';
  const TAG_RE = /^v(\d+)\.(\d+)\.(\d+)(?:-(alpha|beta|rc)\.(\d+))?$/;

  // Returns {channel, core:[X,Y,Z], number} or null for a malformed tag.
  function parseTag(tag) {
    const m = TAG_RE.exec(tag || '');
    if (!m) return null;
    return { channel: m[4] || 'stable', core: [+m[1], +m[2], +m[3]], number: m[5] ? +m[5] : 0 };
  }

  // Channel of a release entry: the manifest's `channel` field when
  // present (versions-all.json), otherwise derived from the tag; legacy
  // entries without either are stable.
  function channelOf(rel) {
    if (rel && rel.channel && CHANNELS.includes(rel.channel)) return rel.channel;
    const t = parseTag(rel && rel.tag);
    return t ? t.channel : 'stable';
  }

  function visibleChannels(selected) {
    const i = CHANNELS.indexOf(selected);
    return CHANNELS.slice(i < 0 ? CHANNELS.length - 1 : i);
  }

  // Entries visible when the user picked `channel`, in the manifest's
  // (newest-first) order.
  function filterReleases(list, channel) {
    const vis = visibleChannels(channel);
    return (list || []).filter(r => vis.includes(channelOf(r)));
  }

  // Dropdown label: pre-releases carry a bracketed channel badge.
  function releaseLabel(rel) {
    const ch = channelOf(rel);
    return ch === 'stable' ? rel.tag : `${rel.tag}  [${ch}]`;
  }

  function loadChannelPref() {
    try {
      const v = localStorage.getItem(CHANNEL_PREF_KEY);
      return CHANNELS.includes(v) ? v : 'stable';
    } catch (e) { return 'stable'; }
  }
  function saveChannelPref(channel) {
    try { localStorage.setItem(CHANNEL_PREF_KEY, channel); } catch (e) { /* private mode */ }
  }

  // Fills a <select> with the channels (most stable first) and selects
  // `current`.
  function populateChannelSelect(sel, current) {
    sel.innerHTML = '';
    for (const ch of [...CHANNELS].reverse()) {
      const o = document.createElement('option');
      o.value = ch;
      o.textContent = CHANNEL_LABELS[ch];
      sel.appendChild(o);
    }
    sel.value = CHANNELS.includes(current) ? current : 'stable';
  }

  // The channel a running firmware belongs to, so a device on an alpha
  // build defaults to seeing the next alpha. Stable / unknown → 'stable'.
  function channelForFirmware(fwTag) {
    const t = parseTag(fwTag);
    return t ? t.channel : 'stable';
  }

  // Least stable of two channels (used to widen a default).
  function widerChannel(a, b) {
    const ia = CHANNELS.indexOf(a), ib = CHANNELS.indexOf(b);
    if (ia < 0) return b;
    if (ib < 0) return a;
    return ia < ib ? a : b;
  }

  // Fetches the release manifest from the Pages site (same origin as the
  // hosted pages). Prefers versions-all.json (every channel, has
  // `channel`); falls back to versions.json (stable only) for a mirror
  // that predates the channel split.
  async function fetchReleaseManifest(base) {
    const b = base === undefined ? './' : base;
    for (const name of ['versions-all.json', 'versions.json']) {
      try {
        const r = await fetch(b + name, { cache: 'no-cache' });
        if (!r.ok) continue;
        const arr = await r.json();
        if (Array.isArray(arr)) return arr;
      } catch (e) { /* try next */ }
    }
    throw new Error('release manifest unavailable');
  }

  window.StackchanSettings = { BOARDS, boardLabel, boardSlug, log, setupTabs, init,
                               setupDslEditor, SERVO_DEFAULTS, setupServoCalibration,
                               setupSecretFields, buildLtConfigJson, seedLtConfigForm,
                               jttsPhrasesFromText, jttsPhrasesToText,
                               CHANNELS, CHANNEL_LABELS, parseTag, channelOf, visibleChannels,
                               filterReleases, releaseLabel, loadChannelPref, saveChannelPref,
                               populateChannelSelect, channelForFirmware, widerChannel,
                               fetchReleaseManifest,
                               proxFormValues, proxFillForm, proxShowLive, proxLivePolling,
                               proxLiveStart, proxLiveStop,
                               proxInsertSection, proxSetVisible,
                               proxTuningValues, proxFillTuning, proxSetupTuning, proxSetupApply };
})();
