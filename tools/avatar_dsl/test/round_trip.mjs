// Verifies that what JS compiles == what C++ decoder expects:
//   - header magic/version are correct
//   - fn table entries point inside the code section
//   - all opcodes used belong to the documented set (no accidental allocations)
//   - smoke-trace the bytecode by hand-decoding selected ops.
import { compile } from '../compile.js';
import { Op, MAGIC, VERSION } from '../opcodes.js';

const KNOWN_OPS = new Set(Object.values(Op));

const src = `
fn eye(cx, cy, r, is_left)
  let x = cx + gaze_h * 3
  let y = cy + gaze_v * 3 + breath * 3
  begin_group(cx - r - 16, cy - r - 16, (r + 16) * 2, (r + 16) * 2)
  if eye_open <= 0 then
    fill_rect(x - r, y - 2, r * 2, 4, primary)
  else
    fill_circle(x, y, r, primary)
    if expr == ANGRY or expr == SAD then
      let flip = is_left ~= (expr == ANGRY)
      let x2 = x - r
      if not flip then x2 = x + r end
      fill_triangle(x - r, y - r, x + r, y - r, x2, y, background)
    elif expr == HAPPY then
      fill_rect(x - r, y, r * 2 + 4, r + 2, background)
      fill_circle(x, y, r / 1.5, background)
    elif expr == SLEEPY then
      fill_rect(x - r, y - r, r * 2 + 4, r + 2, background)
    end
  end
  end_group()
end

fn draw()
  eye(tx(90),  ty(93),  sz(eye_radius), 1)
  eye(tx(230), ty(96),  sz(eye_radius), 0)
end
`;

const buf = compile(src);
const dv = new DataView(buf);
const u8 = new Uint8Array(buf);
if (dv.getUint32(0, true) !== MAGIC) throw new Error('magic');
if (dv.getUint16(4, true) !== VERSION) throw new Error('version');
const constCount = dv.getUint16(8, true);
const fnCount = dv.getUint16(10, true);
const codeSize = dv.getUint16(12, true);
const entry = dv.getUint16(14, true);
console.log({ size: buf.byteLength, constCount, fnCount, codeSize, entry });
if (entry !== 0) throw new Error('entry should be 0 = draw');
if (fnCount !== 2) throw new Error('expected 2 fns');

// Walk the fn table and verify all offsets are < codeSize.
let off = 16;
// skip const table
for (let i = 0; i < constCount; ++i) {
  const tag = u8[off++];
  if (tag === 1) off += 4;       // f32
  else if (tag === 2) off += 4;  // i32
  else if (tag === 3) off += 2;  // color
  else throw new Error(`unknown const tag ${tag}`);
}
const fnTableOff = off;
for (let i = 0; i < fnCount; ++i) {
  const o = dv.getUint16(fnTableOff + i * 6, true);
  const p = u8[fnTableOff + i * 6 + 2];
  const l = u8[fnTableOff + i * 6 + 3];
  console.log(`  fn[${i}] offset=${o} params=${p} locals=${l}`);
  if (o >= codeSize) throw new Error(`fn ${i} offset out of bounds`);
  if (l < p) throw new Error(`fn ${i} local_count < param_count`);
}
const codeOff = fnTableOff + fnCount * 6;
// All opcodes must be known.
const code = u8.subarray(codeOff, codeOff + codeSize);
let pc = 0;
while (pc < code.length) {
  const op = code[pc++];
  if (!KNOWN_OPS.has(op)) throw new Error(`unknown opcode 0x${op.toString(16)} at ${pc - 1}`);
  // skip operand bytes per opcode
  switch (op) {
    case Op.PushF32: pc += 4; break;
    case Op.PushI8:
    case Op.PushConst:
    case Op.PushVar:
    case Op.PushLocal:
    case Op.StoreLocal:
    case Op.Call:
      pc += 1; break;
    case Op.PushI16:
    case Op.Jmp:
    case Op.Jz:
    case Op.Jnz:
      pc += 2; break;
    default: break;
  }
}
if (pc !== code.length) throw new Error(`decoder ran off code: pc=${pc}, len=${code.length}`);
console.log('all opcodes known, fn table OK, code section consistent');
