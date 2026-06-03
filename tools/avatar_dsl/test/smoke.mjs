// Compile a trivial program and round-trip the header / one drawing op.
import { compile } from '../compile.js';

const src = `
fn helper(r)
  return
end

fn draw()
  let r = 8
  begin_group(0, 0, 320, 240)
  fill_circle(tx(160), ty(120), r, primary)
  end_group()
end
`;

const buf = compile(src);
const u8 = new Uint8Array(buf);
const dv = new DataView(buf);
const magic = dv.getUint32(0, true);
const version = dv.getUint16(4, true);
console.log('magic =', magic.toString(16), 'version =', version, 'size =', buf.byteLength);
if (magic !== 0x53445641) throw new Error('bad magic');
if (version !== 1) throw new Error('bad version');
console.log('OK');
