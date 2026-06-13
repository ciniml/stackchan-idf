#!/usr/bin/env bun
// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Remote flasher host server.
//
// Bridges a local idf.py-style POST /flash request to a remote browser tab
// that holds a WebSerial handle on the actual ESP32. The browser runs
// esptool-js; this server is just the relay.
//
// Topology:
//
//   idf.py wrapper / curl  --POST /flash (multipart)-->  this server
//                                                          | WebSocket /ws
//                                                          v
//                                                  remote browser tab
//                                                  (WebSerial + esptool-js)
//                                                          |
//                                                          v
//                                                    ESP32 (CoreS3)
//
// Exactly one browser is accepted at a time (a second connection supersedes
// the first), and exactly one flash runs at a time (a second POST gets 409).

const PORT = Number(process.env.PORT ?? 8765);
const HOST = process.env.HOST ?? '0.0.0.0';
const FLASH_TIMEOUT_MS = 5 * 60 * 1000;

// Static UI is served from ../web relative to this script. Resolving once at
// import time so each request just does a Bun.file() lookup.
const WEB_ROOT = new URL('../web/', import.meta.url).pathname;

// ----- types -----------------------------------------------------------------

interface SectionSpec {
    name: string;
    offset: string; // hex string, e.g. "0x10000"
    size: number; // bytes
}

interface FlashJob {
    id: string;
    sections: SectionSpec[];
    chip: string;
    baud: number;
    erase: boolean;
    // Per-section state, indexed by name.
    written: Map<string, number>;
    totals: Map<string, number>;
    // Progress sink — the SSE response writer pushes events here.
    pushEvent: (line: string) => void;
    // Resolved when browser sends final done frame OR on error/timeout.
    finish: (result: { success: boolean; error?: string }) => void;
    finished: boolean;
    timeoutHandle: ReturnType<typeof setTimeout>;
}

interface BrowserClient {
    ws: import('bun').ServerWebSocket<unknown>;
    since: Date;
    hello?: { userAgent?: string; portInfo?: unknown };
}

interface MonitorSubscriber {
    controller: ReadableStreamDefaultController<Uint8Array>;
    since: Date;
}

// ----- module state ----------------------------------------------------------

let browser: BrowserClient | null = null;
let activeJob: FlashJob | null = null;
let pingTimer: ReturnType<typeof setInterval> | null = null;

// Serial monitor fan-out. The browser pushes each ESP serial line via WS
// `{type:"serial", data:"..."}`; we keep a small ring (so a newly attached
// SSE client sees the immediate past — vital when a crash flushes its
// backtrace 5 ms before the user hits Ctrl+C / re-runs curl) and broadcast
// to every active /monitor SSE subscriber.
const MONITOR_RING_LIMIT = 200;
const monitorRing: string[] = [];
const monitorSubs = new Set<MonitorSubscriber>();
const monitorEncoder = new TextEncoder();

// ----- helpers ---------------------------------------------------------------

function log(msg: string): void {
    const ts = new Date().toISOString();
    console.log(`[${ts}] ${msg}`);
}

function jsonResponse(status: number, body: unknown): Response {
    return new Response(JSON.stringify(body), {
        status,
        headers: { 'Content-Type': 'application/json' },
    });
}

function sendWs(text: string): boolean {
    if (!browser) return false;
    try {
        browser.ws.send(text);
        return true;
    } catch (e) {
        log(`ws send failed: ${(e as Error).message}`);
        return false;
    }
}

function sendWsBinary(buf: ArrayBuffer | Uint8Array): boolean {
    if (!browser) return false;
    try {
        browser.ws.send(buf as Uint8Array);
        return true;
    } catch (e) {
        log(`ws send (binary) failed: ${(e as Error).message}`);
        return false;
    }
}

function statusHtml(): string {
    const connected = browser
        ? `接続中: ${browser.since.toISOString()}` +
          (browser.hello?.userAgent ? ` (UA=${escapeHtml(browser.hello.userAgent)})` : '')
        : 'ブラウザ クライアント未接続';
    const flashing = activeJob ? `flashing job ${activeJob.id}` : 'idle';
    return `<!doctype html>
<html lang="ja"><meta charset="utf-8">
<title>stackchan-idf remote-flasher</title>
<style>
 body{font-family:system-ui,sans-serif;max-width:760px;margin:2em auto;padding:0 1em;color:#222}
 code,pre{background:#f4f4f4;padding:.1em .3em;border-radius:3px}
 pre{padding:.8em;overflow:auto}
 .ok{color:#0a0}.warn{color:#a60}
</style>
<h1>stackchan-idf remote-flasher</h1>
<p>Listening on <code>${HOST}:${PORT}</code></p>
<p>Browser: <strong class="${browser ? 'ok' : 'warn'}">${connected}</strong></p>
<p>Flash state: <strong>${flashing}</strong></p>
<h2>使い方</h2>
<ol>
 <li>ESP32 を USB 接続したマシンの Chrome/Edge で <a href="/app/"><code>/app/</code></a> を開き、Connect Serial で WebSerial 接続。
  <br><span class="warn">WebSerial は secure context 必須 — <code>http://localhost</code> もしくは HTTPS でしか動かない。VPN 越しの IP 直接アクセスでは Chrome が API を無効化する。</span></li>
 <li>idf.py ラッパから次の curl で書き込み開始。</li>
</ol>
<pre>curl -N \\
  -F 'meta={"chip":"esp32s3","baud":460800,"erase":false,"sections":[
       {"name":"bootloader","offset":"0x0"},
       {"name":"partition-table","offset":"0x8000"},
       {"name":"app","offset":"0x10000"}]};type=application/json' \\
  -F 'bootloader=@build/bootloader/bootloader.bin' \\
  -F 'partition-table=@build/partition_table/partition-table.bin' \\
  -F 'app=@build/stackchan_idf.bin' \\
  http://${HOST === '0.0.0.0' ? 'localhost' : HOST}:${PORT}/flash</pre>
<p>レスポンスは <code>text/event-stream</code>。各 section の進捗と最後の <code>done</code> を流す。</p>
<p>リセットだけしたい時: <code>curl -X POST http://localhost:${PORT}/reset</code></p>
<p>シリアル モニタ購読 (browser タブで Monitor を開始してから):
<code>curl -sN http://localhost:${PORT}/monitor</code>。
リング (直近 ${MONITOR_RING_LIMIT} 行) を初回 push してから生流送。</p>
`;
}

function broadcastMonitor(line: string): void {
    // Drop trailing CR/LF so each SSE `data:` event is exactly one logical
    // line; the browser side has already split on \n.
    const clean = line.replace(/\r?\n$/, '');
    monitorRing.push(clean);
    while (monitorRing.length > MONITOR_RING_LIMIT) monitorRing.shift();
    const frame = monitorEncoder.encode(`data: ${clean}\n\n`);
    for (const sub of monitorSubs) {
        try {
            sub.controller.enqueue(frame);
        } catch {
            monitorSubs.delete(sub);
        }
    }
}

function handleMonitorSubscribe(): Response {
    let controller!: ReadableStreamDefaultController<Uint8Array>;
    const stream = new ReadableStream<Uint8Array>({
        start(c) {
            controller = c;
            // Comment frame so curl flushes its first read promptly even
            // before any ESP output arrives.
            controller.enqueue(monitorEncoder.encode(': monitor stream open\n\n'));
            // Replay the ring so a fresh subscriber sees recent context
            // (e.g. a backtrace that just landed). One event per line keeps
            // the parse trivial on the client side.
            for (const line of monitorRing) {
                controller.enqueue(monitorEncoder.encode(`data: ${line}\n\n`));
            }
            const sub: MonitorSubscriber = { controller, since: new Date() };
            monitorSubs.add(sub);
            log(`monitor: subscriber added (now ${monitorSubs.size})`);
        },
        cancel() {
            for (const sub of monitorSubs) {
                if (sub.controller === controller) {
                    monitorSubs.delete(sub);
                    break;
                }
            }
            log(`monitor: subscriber removed (now ${monitorSubs.size})`);
        },
    });
    return new Response(stream, {
        status: 200,
        headers: {
            'Content-Type': 'text/event-stream; charset=utf-8',
            'Cache-Control': 'no-cache, no-transform',
            Connection: 'keep-alive',
            'X-Accel-Buffering': 'no',
        },
    });
}

async function serveStatic(rel: string): Promise<Response> {
    // Default to index.html for the directory root.
    let relPath = rel === '/' || rel === '' ? '/index.html' : rel;
    // Reject anything that would escape WEB_ROOT. We only accept paths made of
    // /-separated path segments that match [A-Za-z0-9._-]+, which excludes
    // "..", absolute paths, NULs, and percent-decoded surprises.
    const segments = relPath.replace(/^\/+/, '').split('/');
    if (segments.some((s) => s.length === 0 || /[^A-Za-z0-9._-]/.test(s) || s === '..' || s.startsWith('.'))) {
        return new Response('forbidden', { status: 403 });
    }
    const full = WEB_ROOT + segments.join('/');
    const f = Bun.file(full);
    if (!(await f.exists())) return new Response('not found', { status: 404 });
    return new Response(f);
}

function escapeHtml(s: string): string {
    return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!);
}

// ----- multipart parsing -----------------------------------------------------

interface ParsedMeta {
    chip: string;
    baud: number;
    erase: boolean;
    sections: { name: string; offset: string }[];
}

function parseMeta(raw: unknown): ParsedMeta {
    if (typeof raw !== 'string') throw new Error('meta must be a JSON string field');
    const obj = JSON.parse(raw) as Record<string, unknown>;
    const chip = String(obj.chip ?? 'esp32s3');
    const baud = Number(obj.baud ?? 460800);
    const erase = Boolean(obj.erase ?? false);
    const sectionsRaw = obj.sections;
    if (!Array.isArray(sectionsRaw) || sectionsRaw.length === 0) {
        throw new Error('meta.sections must be a non-empty array of {name, offset}');
    }
    const sections = sectionsRaw.map((s, i) => {
        const sec = s as Record<string, unknown>;
        const name = String(sec.name ?? '');
        const offset = String(sec.offset ?? '');
        if (!name) throw new Error(`meta.sections[${i}].name is required`);
        if (!/^0x[0-9a-fA-F]+$/.test(offset) && !/^\d+$/.test(offset)) {
            throw new Error(`meta.sections[${i}].offset must be a decimal or 0x-prefixed hex string`);
        }
        return { name, offset };
    });
    return { chip, baud, erase, sections };
}

// ----- flash orchestrator ----------------------------------------------------

async function runFlash(
    job: FlashJob,
    files: { name: string; buf: Uint8Array }[],
): Promise<void> {
    log(`flash ${job.id} start: chip=${job.chip} baud=${job.baud} erase=${job.erase} sections=${job.sections.length}`);
    // Send the flash_request header frame.
    const header = {
        type: 'flash_request',
        id: job.id,
        sections: job.sections.map((s) => ({ name: s.name, offset: s.offset, size: s.size })),
        chip: job.chip,
        baud: job.baud,
        erase: job.erase,
    };
    if (!sendWs(JSON.stringify(header))) {
        job.finish({ success: false, error: 'browser disconnected before flash start' });
        return;
    }
    // Send each section as a single binary frame, in declared order.
    for (const f of files) {
        if (!sendWsBinary(f.buf)) {
            job.finish({ success: false, error: `browser disconnected while sending ${f.name}` });
            return;
        }
    }
    log(`flash ${job.id}: ${files.length} sections uploaded to browser`);
}

function handleBrowserMessage(raw: string | Buffer): void {
    if (typeof raw !== 'string') return; // binary from browser is unexpected
    let msg: Record<string, unknown>;
    try {
        msg = JSON.parse(raw);
    } catch {
        log(`ws: malformed JSON from browser: ${raw.slice(0, 120)}`);
        return;
    }
    const type = msg.type;
    switch (type) {
        case 'hello': {
            if (browser) {
                browser.hello = {
                    userAgent: typeof msg.userAgent === 'string' ? msg.userAgent : undefined,
                    portInfo: msg.portInfo,
                };
                log(`ws hello: ${browser.hello.userAgent ?? '(no UA)'}`);
            }
            return;
        }
        case 'pong':
            return;
        case 'log': {
            const lvl = String(msg.level ?? 'info');
            const m = String(msg.message ?? '');
            log(`[browser:${lvl}] ${m}`);
            return;
        }
        case 'serial': {
            // Pass-through one line of ESP serial output. The browser is
            // expected to have split on \n already so each `data` is one
            // line; we cap the size defensively to keep a runaway publisher
            // from blowing memory on the SSE side.
            const data = typeof msg.data === 'string' ? msg.data : '';
            if (!data) return;
            broadcastMonitor(data.length > 4096 ? data.slice(0, 4096) + '…[truncated]' : data);
            return;
        }
        case 'progress': {
            if (!activeJob || msg.id !== activeJob.id) return;
            const section = String(msg.section ?? '');
            const written = Number(msg.written ?? 0);
            const total = Number(msg.total ?? activeJob.totals.get(section) ?? 0);
            const prev = activeJob.written.get(section) ?? 0;
            activeJob.written.set(section, written);
            if (total > 0) activeJob.totals.set(section, total);
            // 10%-step stdout log.
            if (total > 0) {
                const prevDecile = Math.floor((prev * 10) / total);
                const newDecile = Math.floor((written * 10) / total);
                if (newDecile > prevDecile) {
                    log(`flash ${activeJob.id} ${section}: ${newDecile * 10}% (${written}/${total})`);
                }
            }
            activeJob.pushEvent(
                `data: ${JSON.stringify({ type: 'progress', section, written, total })}\n\n`,
            );
            return;
        }
        case 'done': {
            if (!activeJob || msg.id !== activeJob.id) return;
            const success = Boolean(msg.success);
            const error = typeof msg.error === 'string' ? msg.error : undefined;
            log(`flash ${activeJob.id} done: success=${success}${error ? ` error=${error}` : ''}`);
            activeJob.finish({ success, error });
            return;
        }
        default:
            log(`ws: unknown message type ${String(type)}`);
    }
}

function disconnectBrowser(reason: string): void {
    if (!browser) return;
    log(`browser disconnect: ${reason}`);
    browser = null;
    // If a flash is mid-flight, fail it.
    if (activeJob) {
        activeJob.finish({ success: false, error: `browser disconnected: ${reason}` });
    }
}

// ----- ping loop -------------------------------------------------------------

function ensurePingTimer(): void {
    if (pingTimer) return;
    pingTimer = setInterval(() => {
        if (browser) sendWs(JSON.stringify({ type: 'ping' }));
    }, 15_000);
}

// ----- server ----------------------------------------------------------------

const server = Bun.serve({
    port: PORT,
    hostname: HOST,
    // Allow large multipart uploads (firmware binary ~ 1-2 MB).
    maxRequestBodySize: 32 * 1024 * 1024,
    async fetch(req, srv): Promise<Response | undefined> {
        const url = new URL(req.url);
        if (url.pathname === '/' && req.method === 'GET') {
            return new Response(statusHtml(), { headers: { 'Content-Type': 'text/html; charset=utf-8' } });
        }
        if (url.pathname === '/ws') {
            if (srv.upgrade(req)) return undefined;
            return new Response('expected websocket upgrade', { status: 426 });
        }
        if (url.pathname === '/flash' && req.method === 'POST') {
            return handleFlashRequest(req);
        }
        if (url.pathname === '/reset' && req.method === 'POST') {
            if (!browser) return jsonResponse(503, { error: 'no browser connected' });
            sendWs(JSON.stringify({ type: 'reset' }));
            return jsonResponse(200, { ok: true });
        }
        if (url.pathname === '/monitor' && req.method === 'GET') {
            return handleMonitorSubscribe();
        }
        if ((url.pathname === '/app' || url.pathname.startsWith('/app/')) && req.method === 'GET') {
            return serveStatic(url.pathname.slice('/app'.length) || '/');
        }
        return new Response('not found', { status: 404 });
    },
    websocket: {
        open(ws) {
            if (browser) {
                // Supersede the existing connection.
                log('ws: superseding previous browser connection');
                try {
                    browser.ws.send(JSON.stringify({ type: 'superseded' }));
                } catch {
                    /* ignore */
                }
                try {
                    browser.ws.close();
                } catch {
                    /* ignore */
                }
            }
            browser = { ws, since: new Date() };
            log(`ws: browser connected (${browser.since.toISOString()})`);
            ensurePingTimer();
        },
        message(ws, message) {
            if (!browser || browser.ws !== ws) return;
            handleBrowserMessage(message as string | Buffer);
        },
        close(ws, code, reason) {
            if (browser && browser.ws === ws) {
                disconnectBrowser(`code=${code} reason=${reason || '(empty)'}`);
            }
        },
    },
});

// ----- POST /flash -----------------------------------------------------------

async function handleFlashRequest(req: Request): Promise<Response> {
    if (!browser) return jsonResponse(503, { error: 'no browser connected' });
    if (activeJob) return jsonResponse(409, { error: 'flash in progress' });

    let form: FormData;
    try {
        form = await req.formData();
    } catch (e) {
        return jsonResponse(400, { error: `multipart parse failed: ${(e as Error).message}` });
    }

    let meta: ParsedMeta;
    try {
        meta = parseMeta(form.get('meta'));
    } catch (e) {
        return jsonResponse(400, { error: `meta: ${(e as Error).message}` });
    }

    // Resolve each declared section against a multipart file part whose field
    // name matches `section.name`. Read into Uint8Array up front so the
    // browser sees one binary frame per section.
    const files: { name: string; buf: Uint8Array }[] = [];
    const fullSections: SectionSpec[] = [];
    for (const sec of meta.sections) {
        const part = form.get(sec.name);
        if (!(part instanceof Blob)) {
            return jsonResponse(400, { error: `missing file field for section "${sec.name}"` });
        }
        const buf = new Uint8Array(await part.arrayBuffer());
        files.push({ name: sec.name, buf });
        fullSections.push({ name: sec.name, offset: sec.offset, size: buf.byteLength });
    }

    const id = crypto.randomUUID();

    // SSE response. We synthesize the stream with a ReadableStream so we can
    // push events as the browser reports progress.
    let controller!: ReadableStreamDefaultController<Uint8Array>;
    const encoder = new TextEncoder();
    const stream = new ReadableStream<Uint8Array>({
        start(c) {
            controller = c;
        },
        cancel() {
            // Client gave up. If flash still active, mark it failed so the
            // next POST can proceed.
            if (activeJob && activeJob.id === id) {
                activeJob.finish({ success: false, error: 'client cancelled' });
            }
        },
    });

    const pushEvent = (line: string) => {
        try {
            controller.enqueue(encoder.encode(line));
        } catch {
            /* stream closed; ignore */
        }
    };

    const job: FlashJob = {
        id,
        sections: fullSections,
        chip: meta.chip,
        baud: meta.baud,
        erase: meta.erase,
        written: new Map(),
        totals: new Map(fullSections.map((s) => [s.name, s.size])),
        pushEvent,
        finished: false,
        timeoutHandle: setTimeout(() => {
            if (activeJob && activeJob.id === id) {
                log(`flash ${id} timeout`);
                activeJob.finish({ success: false, error: 'timeout' });
            }
        }, FLASH_TIMEOUT_MS),
        finish: (result) => {
            if (job.finished) return;
            job.finished = true;
            clearTimeout(job.timeoutHandle);
            pushEvent(`data: ${JSON.stringify({ type: 'done', success: result.success, error: result.error })}\n\n`);
            try {
                controller.close();
            } catch {
                /* already closed */
            }
            if (activeJob === job) activeJob = null;
        },
    };

    activeJob = job;
    // Kick off the upload to browser.
    void runFlash(job, files);

    // We can only return one HTTP status. Streaming the body means we commit
    // to 200 here; embed `success:false` in the final `done` event for the
    // failure case. (Strict-spec requirement said "500 on failure" — we honor
    // that for the synchronous reject cases above where we can still set the
    // status, but for runtime failures during the stream the browser-side
    // error message is in the SSE `done` payload.)
    return new Response(stream, {
        status: 200,
        headers: {
            'Content-Type': 'text/event-stream; charset=utf-8',
            'Cache-Control': 'no-cache, no-transform',
            Connection: 'keep-alive',
            'X-Accel-Buffering': 'no',
        },
    });
}

log(`remote-flasher server listening on http://${HOST}:${PORT}`);
log(`status page: http://${HOST === '0.0.0.0' ? 'localhost' : HOST}:${PORT}/`);
log(`websocket:   ws://${HOST === '0.0.0.0' ? 'localhost' : HOST}:${PORT}/ws`);

// Keep TS happy about unused server var.
void server;
