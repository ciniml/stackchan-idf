#!/usr/bin/env bun
// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Stack-chan Channel adapter for Claude Code. Spawned by `claude` as a
// subprocess via the project's `.mcp.json` and connected over stdio. Exposes
// four tools that map 1:1 to the firmware's /mcp/* REST endpoints:
//
//   say          → POST /mcp/say          (text body = hiragana/katakana)
//   set_expression → POST /mcp/expression (text body = expression name)
//   set_balloon  → POST /mcp/balloon?hold_ms=N (text body = display text)
//   get_state    → GET  /mcp/state        (JSON response)
//
// All HTTP calls carry `Authorization: Bearer <STACKCHAN_TOKEN>`. The Stack-chan
// URL is typically a Cloudflare Tunnel hostname (HTTPS), but a LAN HTTP URL
// works too for at-home testing.
//
// Configuration via env vars (set in your .env or shell):
//   STACKCHAN_URL    e.g. https://stackchan.example.com   (required)
//   STACKCHAN_TOKEN  matches CONFIG_MCP_API_TOKEN on the firmware (required)
//
// Phase 2 adds push: a background SSE reader on GET /mcp/events relays
// device-initiated events (boot, touch stroke, say_done, conversation_state)
// to Claude as `notifications/claude/channel/event` so they appear in the
// Claude Code conversation without polling.

import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from '@modelcontextprotocol/sdk/types.js';

const STACKCHAN_URL = process.env.STACKCHAN_URL?.replace(/\/$/, '');
const STACKCHAN_TOKEN = process.env.STACKCHAN_TOKEN ?? '';
if (!STACKCHAN_URL || !STACKCHAN_TOKEN) {
  // Fatal: refuse to start. Without these the tools would 401 every time.
  console.error('stackchan-channel: STACKCHAN_URL and STACKCHAN_TOKEN env vars are required');
  process.exit(1);
}

const EXPRESSION_NAMES = ['neutral', 'happy', 'sad', 'angry', 'doubt', 'sleepy'] as const;
type ExpressionName = (typeof EXPRESSION_NAMES)[number];

async function callFirmware(
  method: 'GET' | 'POST',
  path: string,
  body?: string,
): Promise<{ status: number; text: string }> {
  const headers: Record<string, string> = {
    Authorization: `Bearer ${STACKCHAN_TOKEN}`,
    // streamEvents() holds a long-poll GET /mcp/events open against the
    // same host. Bun's fetch keep-alive pool can otherwise wedge a regular
    // /mcp/say POST behind the SSE socket and the call hangs until Claude
    // SIGINTs at ~50 s. `Connection: close` forces a fresh TCP socket per
    // call (also nicer to ESP-IDF httpd's tiny max_open_sockets pool).
    Connection: 'close',
  };
  if (body !== undefined) headers['Content-Type'] = 'text/plain; charset=utf-8';
  const r = await fetch(`${STACKCHAN_URL}${path}`, {
    method,
    headers,
    body,
    keepalive: false,
  });
  const text = await r.text();
  return { status: r.status, text };
}

function errorContent(message: string) {
  return { content: [{ type: 'text' as const, text: message }], isError: true };
}

function okContent(text: string) {
  return { content: [{ type: 'text' as const, text }] };
}

const mcp = new Server(
  { name: 'stackchan', version: '0.2.0' },
  {
    // Channel capability + the push-event background loop below means Claude
    // Code surfaces device-initiated events (boot, touch, say_done,
    // conversation state changes) in-conversation without polling.
    capabilities: {
      experimental: { 'claude/channel': {} },
      tools: {},
    },
    instructions:
      'Stack-chan は M5Stack 製の卓上ロボット。アバター顔の表情・吹き出し・' +
      '音声合成 (jtts) を操作できる。`say` のテキストはひらがな必須 ' +
      '(漢字→読み変換は無し)。表情・吹き出しはユーザーの邪魔にならない範囲で。' +
      '状態を確認したいときは get_state。device からは boot / touch / say_done ' +
      '/ conversation_state イベントが Channel 通知として届く。',
  },
);

// --- SSE push-event consumer -------------------------------------------------
//
// Holds a long-poll GET /mcp/events open. Each `event: <type>` / `data: <json>`
// pair is forwarded to Claude as `notifications/claude/channel/event`.
// Reconnects with exponential backoff on disconnect — Cloudflare Tunnel idle-
// closes around 100 s but the firmware emits `: keepalive` every 15 s so a
// healthy connection stays live indefinitely.

interface DeviceEvent {
  type: string;
  data: unknown;
}

async function emitChannelEvent(ev: DeviceEvent): Promise<void> {
  try {
    // Claude Code Channel is experimental; the method name mirrors the
    // capability key we declared above. If Claude Code doesn't recognize
    // the method it silently drops the notification, which is fine — the
    // tools still work.
    await mcp.notification({
      method: 'notifications/claude/channel/event',
      params: { source: 'stackchan', event: ev.type, data: ev.data },
    });
  } catch (e) {
    console.error(`stackchan-channel: emitChannelEvent failed: ${(e as Error).message}`);
  }
}

async function streamEvents(): Promise<void> {
  // Backoff: 1, 2, 4, 8, capped at 30 s. Resets to 1 s on any successful read.
  let backoffMs = 1000;
  for (;;) {
    try {
      const r = await fetch(`${STACKCHAN_URL}/mcp/events`, {
        method: 'GET',
        headers: {
          Authorization: `Bearer ${STACKCHAN_TOKEN}`,
          Accept: 'text/event-stream',
        },
      });
      if (!r.ok || !r.body) {
        // 404 = firmware has /mcp/* disabled (empty token Kconfig). No point
        // hammering it — wait a long while then re-check in case the user
        // flashed a build with the API enabled.
        const waitMs = r.status === 404 ? 60_000 : backoffMs;
        console.error(`stackchan-channel: /mcp/events HTTP ${r.status}, retrying in ${waitMs} ms`);
        await new Promise((res) => setTimeout(res, waitMs));
        backoffMs = Math.min(backoffMs * 2, 30_000);
        continue;
      }
      backoffMs = 1000;
      const reader = r.body.getReader();
      const decoder = new TextDecoder();
      let buf = '';
      let currentEvent = '';
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += decoder.decode(value, { stream: true });
        // SSE is line-oriented with blank-line frame separators. Parse on each
        // \n; the buf carries any half-line into the next chunk.
        let nl;
        while ((nl = buf.indexOf('\n')) >= 0) {
          const line = buf.slice(0, nl).replace(/\r$/, '');
          buf = buf.slice(nl + 1);
          if (line === '') {
            currentEvent = '';
            continue;
          }
          if (line.startsWith(':')) continue; // comment / keepalive
          if (line.startsWith('event:')) {
            currentEvent = line.slice(6).trim();
          } else if (line.startsWith('data:')) {
            const payload = line.slice(5).trim();
            let parsed: unknown = payload;
            try {
              parsed = JSON.parse(payload);
            } catch {
              // Leave as raw string; Claude still gets something useful.
            }
            await emitChannelEvent({ type: currentEvent || 'unknown', data: parsed });
          }
        }
      }
      // Server closed cleanly — reconnect immediately.
      console.error('stackchan-channel: /mcp/events stream closed, reconnecting');
    } catch (e) {
      console.error(
        `stackchan-channel: /mcp/events error (${(e as Error).message}), retry in ${backoffMs} ms`,
      );
      await new Promise((res) => setTimeout(res, backoffMs));
      backoffMs = Math.min(backoffMs * 2, 30_000);
    }
  }
}

mcp.setRequestHandler(ListToolsRequestSchema, async () => ({
  tools: [
    {
      name: 'say',
      description:
        'Stack-chan に発話させる。テキストはひらがな。漢字は読み取られない (jtts は読み変換器を持たない)。発話中に再度呼ぶと前の発話完了後にキュー実行。',
      inputSchema: {
        type: 'object',
        properties: {
          text: { type: 'string', maxLength: 200, description: 'ひらがなのテキスト' },
        },
        required: ['text'],
      },
    },
    {
      name: 'set_expression',
      description: 'Stack-chan のアバター表情を変更。',
      inputSchema: {
        type: 'object',
        properties: {
          expression: { type: 'string', enum: EXPRESSION_NAMES as unknown as string[] },
        },
        required: ['expression'],
      },
    },
    {
      name: 'set_balloon',
      description:
        'アバター下の吹き出しにテキスト表示。長文はマーキー スクロール。hold_ms 省略時はデフォルト時間。',
      inputSchema: {
        type: 'object',
        properties: {
          text: { type: 'string', maxLength: 200 },
          hold_ms: { type: 'integer', minimum: 0, default: 0 },
        },
        required: ['text'],
      },
    },
    {
      name: 'get_state',
      description: 'Stack-chan の現在状態 (FW バージョン、IP、Wi-Fi、バッテリ、ボード種別) を JSON で返す。',
      inputSchema: { type: 'object', properties: {} },
    },
  ],
}));

mcp.setRequestHandler(CallToolRequestSchema, async (req) => {
  const name = req.params.name;
  const args = (req.params.arguments ?? {}) as Record<string, unknown>;
  try {
    switch (name) {
      case 'say': {
        const text = String(args.text ?? '');
        if (!text) return errorContent('text is required');
        const r = await callFirmware('POST', '/mcp/say', text);
        if (r.status !== 200) return errorContent(`HTTP ${r.status}: ${r.text}`);
        return okContent('queued');
      }
      case 'set_expression': {
        const expr = String(args.expression ?? '');
        if (!EXPRESSION_NAMES.includes(expr as ExpressionName)) {
          return errorContent(`expression must be one of ${EXPRESSION_NAMES.join(', ')}`);
        }
        const r = await callFirmware('POST', '/mcp/expression', expr);
        if (r.status !== 200) return errorContent(`HTTP ${r.status}: ${r.text}`);
        return okContent(`expression=${expr}`);
      }
      case 'set_balloon': {
        const text = String(args.text ?? '');
        const holdMs = Number(args.hold_ms ?? 0);
        if (!text) return errorContent('text is required');
        const path = `/mcp/balloon${holdMs > 0 ? `?hold_ms=${holdMs}` : ''}`;
        const r = await callFirmware('POST', path, text);
        if (r.status !== 200) return errorContent(`HTTP ${r.status}: ${r.text}`);
        return okContent('shown');
      }
      case 'get_state': {
        const r = await callFirmware('GET', '/mcp/state');
        if (r.status !== 200) return errorContent(`HTTP ${r.status}: ${r.text}`);
        // Pass the JSON through so Claude can pick fields out.
        return okContent(r.text);
      }
      default:
        return errorContent(`unknown tool: ${name}`);
    }
  } catch (e) {
    return errorContent(`fetch error: ${e instanceof Error ? e.message : String(e)}`);
  }
});

await mcp.connect(new StdioServerTransport());
// Log to stderr — stdout is reserved for the stdio JSON-RPC transport.
console.error(`stackchan-channel ready (target=${STACKCHAN_URL})`);
// Detached: the SSE loop must not block the stdio handshake. void-cast so
// node's unhandled-rejection logic doesn't yell — we already log internally.
void streamEvents();
