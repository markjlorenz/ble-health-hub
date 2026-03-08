// Vendor-specific Pulse Oximeter service UUID extracted from data/pulse-ox.pcapng
// (also visible in advertisements).
const VENDOR_SERVICE_UUID = "56313100-504f-6e2e-6175-696a2e6d6f63";
// Some stacks/tools display this UUID with a different byte order.
// If the device doesn't expose VENDOR_SERVICE_UUID, we try this alternate form.
const VENDOR_SERVICE_UUID_ALT = "00313156-4f50-2e6e-6175-696a2e6d6f63";
// Wireshark sometimes shows the raw 16 bytes as an ASCII-ish string:
//   636f6d2e6a6975616e2e504f56313100  -> "com.jiuan.POV11\0"
// Interpreting those bytes directly as a UUID yields:
const VENDOR_SERVICE_UUID_WIRESHARK = "636f6d2e-6a69-7561-6e2e-504f56313100";

const GENERIC_ACCESS_UUID = "00001800-0000-1000-8000-00805f9b34fb";
const GENERIC_ATTRIBUTE_UUID = "00001801-0000-1000-8000-00805f9b34fb";

// Observed in data/pulse-ox.pcapng: the phone app sends vendor write commands
// and the device replies with short 6-byte ACK notifications of the form:
//   a0 03 a0 <step> ac ??
// This handshake precedes streaming measurement frames (a0 11 f0 ...).
//
// Rather than blasting all writes back-to-back, we send them in stages and only
// advance when the expected ACK step is observed.
//
// Stages 1 and 2 contain "long write" messages (b0 11 and b0 06) whose trailing
// bytes differ across vendor pcap sessions. These bytes are NOT client-generated
// random nonces that we can simply invent — the device validates them and will
// not ACK stage1 if they are wrong.
//
// Based on inspecting the official Android SDK jar metadata, the PO3 path
// delegates into obfuscated `com/ihealth/communication/ins/*` classes and calls
// into `com/ihealth/communication/ins/GenerateKap`, which loads a native library
// (`System.loadLibrary("iHealth")`). Those classes also reference
// `java/util/Random`, strongly suggesting a per-session RNG nonce is combined
// with a device/app secret in native code to produce the validated tail bytes.
//
// Stages 1 and 2 are generated dynamically from the reverse-engineered
// IdentifyIns/XXTEA + BleCommProtocol framing logic (they vary per session).
const VENDOR_HANDSHAKE_STAGES = [
  {
    name: "stage3",
    completeAckStep: 0x18,
    // The device emits A0 04 at base 0x14 right after ACK 0x12.
    // Its tail bytes are the challenge for the B0 04 write (step = base+3 = 0x17).
    promptBase: 0x14,
    b004Step: 0x17,
    writesHex: ["b003a015ac61", "b0040017acc184"],
  },
  {
    name: "stage4",
    completeAckStep: 0x1e,
    // Device emits A0 04 at base 0x1a right after ACK 0x18.
    promptBase: 0x1a,
    b004Step: 0x1d,
    writesHex: ["b003a01bac67", "b004001dacc68f"],
  },
  {
    name: "stage5",
    completeAckStep: 0x24,
    // Device emits A0 04 at base 0x20 right after ACK 0x1e (capture shows A0 06
    // at this position, but live firmware uses A0 04 — we handle both).
    promptBase: 0x20,
    b004Step: 0x23,
    writesHex: ["b003a021ac6d", "b0040023aca574"],
  },
  {
    name: "stage6",
    completeAckStep: 0x2c,
    writesHex: ["b003a027ac73", "b003a029ac75", "b004002baca67d"],
  },
];

const el = {
  connectBtn: document.getElementById("connectBtn"),
  disconnectBtn: document.getElementById("disconnectBtn"),
  copyLogBtn: document.getElementById("copyLogBtn"),
  status: document.getElementById("status"),
  device: document.getElementById("device"),
  serviceUuid: document.getElementById("serviceUuid"),
  measurementChar: document.getElementById("measurementChar"),
  spo2: document.getElementById("spo2"),
  hr: document.getElementById("hr"),
  strength: document.getElementById("strength"),
  pleth: document.getElementById("pleth"),
  frameMeta: document.getElementById("frameMeta"),
  rawHex: document.getElementById("rawHex"),
  log: document.getElementById("log"),
};

async function copyDebugLogToClipboard() {
  const text = el.log?.textContent ?? "";
  if (!text) return;

  if (navigator.clipboard?.writeText && window.isSecureContext) {
    await navigator.clipboard.writeText(text);
    return;
  }

  // Fallback: execCommand('copy') via a hidden textarea.
  const ta = document.createElement("textarea");
  ta.value = text;
  ta.setAttribute("readonly", "");
  ta.style.position = "fixed";
  ta.style.left = "-9999px";
  ta.style.top = "0";
  document.body.appendChild(ta);
  ta.select();
  document.execCommand("copy");
  document.body.removeChild(ta);
}

const APP_VERSION = "2026-03-05.1";

// --- PO3 Identify / Key Material (reverse engineered from MyVitals 4.13.1) ---
const IDENTIFY_SELECTOR = "PO3";
const IDENTIFY_CMD = 0xac;
const IDENTIFY_STAGE1_MARKER = 0xfa;
const IDENTIFY_STAGE2_MARKER = 0xfc;
const IDENTIFY_F_BYTES = new TextEncoder().encode("Ch/HQ4LzItYT42s=");

// Extracted from vendor/ihealth/myvitals-4.13.1/libiHealth.armeabi-v7a.so
// via tools/extract_libihealth_getkey_blobs.py (validated against data/pulse-ox-2.pcapng)
const PO3_KEY_BLOB16 = new Uint8Array([
  0xbf, 0x4b, 0x05, 0x11, 0x42, 0xd2, 0x70, 0xa3, 0x93, 0x2d, 0x2d, 0xaa, 0xcc,
  0xa9, 0xbd, 0x1e,
]);

// In the original captures with finger already on sensor, the device goes straight
// to streaming after ACK 0x2c with no idle prompts. But when no finger is present,
// the device sends A0 04 prompts (starting at base=0x2e) expecting B0 03+B0 04
// replies (same as handshake stages). Respond immediately — no defer needed.
// Update (2026-03-05): Per pcap review, once the final stage-6 write
//   b0 04 00 2b ac a6 7d
// is sent and the device ACKs
//   a0 03 a0 2c ac 78
// the central should not send *any* further vendor writes. The peripheral will
// either start streaming or remain in its idle prompt loop autonomously.
const POST_HANDSHAKE_PROMPT_DEFER_MS = 0;
const POST_HANDSHAKE_WRITES_ENABLED = false;

let device = null;
let gattServer = null;
let measurementCharacteristic = null;

let activeNotificationChars = [];
let lastRawHex = null;
let lastFrameAtMs = 0;
let firstMeasurementLogged = false;
let vendorHandshakeComplete = false;

// Track a few vendor message types we see (A0 11 xx ...), to aid debugging.
const a011TypeLogCounts = new Map();
const A011_TYPE_LOG_LIMIT = 3;

let handshake = {
  running: false,
  writer: null,
  txSeq: 1,
  expectedAckStep: null,
  ackResolver: null,
  ackSeenSteps: new Set(),
  expectedPromptBase: null,
  promptResolver: null,
  promptSeenByBase: new Map(),
  cancelToken: 0,
};

let vendorRx = {
  buf: new Uint8Array(0),
  reassembly: null,
  identifyBuffered: null,
  identifyResolver: null,
};

let postHandshakePrompt = {
  // Reference: pulse-ox-3 vendor capture (finger-loss section).
  // In the idle loop the vendor app sends B0 03 ONLY. The device does NOT ACK
  // these writes — it re-prompts every ~960 ms and transitions to streaming
  // autonomously when a finger is detected. Sending B0 04 here drives the loop
  // at ~90 ms/prompt via the ACK cycle and prevents streaming transition.
  inFlight: false,
  queue: [],
  enqueuedBases: new Set(),
  handledBases: new Set(),
  lastSeenBase: null,
  respondAfterMs: 0,
  deferNoticeLogged: false,
  ignoredBases: new Set(),
};

let nonFrameLogCount = 0;
const NON_FRAME_LOG_LIMIT = 12;

let postHandshakeDebugUntilMs = 0;

// --- Logging (Wireshark-like capture row) ---
// Format: <relative-seconds>  <message>  <direction>  <hex bytes>
// Example:
//   1.234567  Handshake stage1 write  🟦C→P  b0 03 a0 04 ac 50
let logT0Ms = performance.now();
const LOG_COLS = {
  time: 12, // "12345.678901"
  message: 46,
  direction: 22,
};

function formatRelTimeSeconds() {
  const s = (performance.now() - logT0Ms) / 1000;
  return s.toFixed(6);
}

function clampCell(s, width) {
  if (!s) return "".padEnd(width, " ");
  if (s.length === width) return s;
  if (s.length < width) return s.padEnd(width, " ");
  // Truncate with an ellipsis (Wireshark-like column feel).
  return s.slice(0, Math.max(0, width - 1)) + "…";
}

function toHexSpaced(u8) {
  if (!(u8 instanceof Uint8Array)) {
    return "";
  }
  return Array.from(u8)
    .map((b) => b.toString(16).padStart(2, "0"))
    .join(" ");
}

function resetLog() {
  logT0Ms = performance.now();
  el.log.textContent = "";
  const header =
    clampCell("Time", LOG_COLS.time) +
    "  " +
    clampCell("Message", LOG_COLS.message) +
    "  " +
    clampCell("Direction", LOG_COLS.direction) +
    "  " +
    "Hex";
  const underline =
    clampCell("-".repeat(LOG_COLS.time), LOG_COLS.time) +
    "  " +
    clampCell("-".repeat(LOG_COLS.message), LOG_COLS.message) +
    "  " +
    clampCell("-".repeat(LOG_COLS.direction), LOG_COLS.direction) +
    "  " +
    "-".repeat(3);
  el.log.textContent += header + "\n" + underline + "\n";

  // Keep a stable "time 0" preamble line for easy copy/paste.
  log(`App version ${APP_VERSION}`, "", "", 0);
}

function setStatus(text) {
  el.status.textContent = text;
}

function log(message, direction = "", value = null, tsOverrideSeconds = null) {
  const tsRaw =
    typeof tsOverrideSeconds === "number"
      ? tsOverrideSeconds.toFixed(6)
      : formatRelTimeSeconds();
  const ts = clampCell(tsRaw, LOG_COLS.time);
  const msgCell = clampCell(String(message ?? ""), LOG_COLS.message);
  const dirCell = clampCell(String(direction ?? ""), LOG_COLS.direction);
  const hex = value instanceof Uint8Array ? toHexSpaced(value) : String(value ?? "");
  el.log.textContent += `${ts}  ${msgCell}  ${dirCell}  ${hex}`.trimEnd() + "\n";
  el.log.scrollTop = el.log.scrollHeight;
}

function toHex(u8) {
  return Array.from(u8)
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

function toAsciiPreview(u8) {
  const max = Math.min(u8.byteLength, 20);
  let s = "";
  for (let i = 0; i < max; i++) {
    const b = u8[i];
    s += b >= 0x20 && b <= 0x7e ? String.fromCharCode(b) : ".";
  }
  return s;
}

function findBytes(haystack, needle, start = 0) {
  if (!(haystack instanceof Uint8Array)) {
    throw new Error("Expected Uint8Array haystack");
  }
  if (!(needle instanceof Uint8Array)) {
    throw new Error("Expected Uint8Array needle");
  }
  if (needle.length === 0) {
    return -1;
  }
  for (let i = Math.max(0, start); i <= haystack.length - needle.length; i++) {
    let ok = true;
    for (let j = 0; j < needle.length; j++) {
      if (haystack[i + j] !== needle[j]) {
        ok = false;
        break;
      }
    }
    if (ok) {
      return i;
    }
  }
  return -1;
}

function fromHex(hex) {
  const normalized = hex.trim().toLowerCase();
  if (normalized.length % 2 !== 0) {
    throw new Error(`Invalid hex length: ${hex.length}`);
  }
  const out = new Uint8Array(normalized.length / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(normalized.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

function concatU8(chunks) {
  const total = chunks.reduce((n, c) => n + c.byteLength, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const c of chunks) {
    out.set(c, off);
    off += c.byteLength;
  }
  return out;
}

function identifyInsC16(in16) {
  if (!(in16 instanceof Uint8Array) || in16.byteLength !== 16) {
    throw new Error(`IdentifyIns.c expects 16 bytes, got ${in16?.byteLength}`);
  }
  const out = new Uint8Array(16);
  for (let i = 0; i < 4; i++) {
    out[i] = in16[3 - i];
    out[i + 4] = in16[7 - i];
    out[i + 8] = in16[11 - i];
    out[i + 12] = in16[15 - i];
  }
  return out;
}

function nibbleSwapBytes(bytes) {
  const out = new Uint8Array(bytes.byteLength);
  for (let i = 0; i < bytes.byteLength; i++) {
    const b = bytes[i];
    out[i] = ((b & 0x0f) << 4) | ((b & 0xf0) >> 4);
  }
  return out;
}

function bytesToInt32BE(bytes) {
  if (bytes.byteLength % 4 !== 0) {
    throw new Error(`Expected multiple of 4 bytes, got ${bytes.byteLength}`);
  }
  const n = bytes.byteLength / 4;
  const out = new Int32Array(n);
  for (let i = 0; i < n; i++) {
    const o = i * 4;
    out[i] =
      (bytes[o] << 24) |
      (bytes[o + 1] << 16) |
      (bytes[o + 2] << 8) |
      bytes[o + 3];
  }
  return out;
}

function int32ToBytesBE(words) {
  const out = new Uint8Array(words.length * 4);
  for (let i = 0; i < words.length; i++) {
    const v = words[i] | 0;
    const o = i * 4;
    out[o] = (v >>> 24) & 0xff;
    out[o + 1] = (v >>> 16) & 0xff;
    out[o + 2] = (v >>> 8) & 0xff;
    out[o + 3] = v & 0xff;
  }
  return out;
}

function xxteaEncryptBytes(dataBytes, keyBytes16) {
  if (!(dataBytes instanceof Uint8Array)) {
    throw new Error("xxteaEncryptBytes expects Uint8Array data");
  }
  if (!(keyBytes16 instanceof Uint8Array) || keyBytes16.byteLength !== 16) {
    throw new Error("XXTEA needs a 128-bits key");
  }
  if (dataBytes.byteLength < 8) {
    // Matches the Java guard: n < 2 (ints) => return unchanged.
    return dataBytes.slice();
  }
  const v = bytesToInt32BE(dataBytes);
  const k = bytesToInt32BE(keyBytes16);

  const n = v.length;
  let rounds = Math.floor(52 / n) + 6;
  const delta = 1640531527 | 0;
  let sum = 0 | 0;
  let z = v[n - 1] | 0;

  while (rounds-- > 0) {
    sum = (sum - delta) | 0;
    const e = (sum >>> 2) & 3;
    for (let p = 0; p < n - 1; p++) {
      const y = v[p + 1] | 0;
      const mx =
        ((((z >>> 5) ^ (y << 2)) + ((y >>> 3) ^ (z << 4))) ^
          ((y ^ sum) + (z ^ k[(p & 3) ^ e]))) | 0;
      v[p] = (v[p] + mx) | 0;
      z = v[p] | 0;
    }
    const y0 = v[0] | 0;
    const mxLast =
      ((((z >>> 5) ^ (y0 << 2)) + ((y0 >>> 3) ^ (z << 4))) ^
        ((y0 ^ sum) + (z ^ k[((n - 1) & 3) ^ e]))) | 0;
    v[n - 1] = (v[n - 1] + mxLast) | 0;
    z = v[n - 1] | 0;
  }

  return int32ToBytesBE(v);
}

function identifyInsGetKa(selector) {
  if (selector !== IDENTIFY_SELECTOR) {
    throw new Error(`Unsupported Identify selector: ${selector}`);
  }
  const swappedKey = nibbleSwapBytes(PO3_KEY_BLOB16);
  const swappedF = nibbleSwapBytes(IDENTIFY_F_BYTES);
  return xxteaEncryptBytes(swappedKey, swappedF);
}

function identifyInsIdentify(cmd) {
  const a = new Uint8Array(16);
  crypto.getRandomValues(a);

  // Match Java byte semantics:
  //   Random.nextBytes(byte[16]) then for each byte: if <0 => b = (byte)(-b)
  // (yes, -128 remains 0x80).
  for (let i = 0; i < 16; i++) {
    let s = a[i];
    if (s >= 128) {
      s = s - 256;
    }
    if (s < 0) {
      s = -s;
    }
    a[i] = s & 0xff;
  }

  const c = identifyInsC16(a);
  const out = new Uint8Array(18);
  out[0] = cmd & 0xff;
  out[1] = IDENTIFY_STAGE1_MARKER;
  out.set(c, 2);
  return out;
}

function identifyInsDeciphering(challenge48, selector, cmd) {
  if (!(challenge48 instanceof Uint8Array) || challenge48.byteLength !== 48) {
    throw new Error(`IdentifyIns.deciphering expects 48 bytes, got ${challenge48?.byteLength}`);
  }

  const d = challenge48.slice(0, 16);
  const b = challenge48.slice(16, 32);
  const c = challenge48.slice(32, 48);

  const t0 = xxteaEncryptBytes(identifyInsC16(d), identifyInsGetKa(selector));
  // Java calls XXTEA.encrypt(c(b), t0) but discards the result.
  void xxteaEncryptBytes(identifyInsC16(b), t0);
  const t2 = xxteaEncryptBytes(identifyInsC16(c), t0);
  const out16 = identifyInsC16(t2);

  const out = new Uint8Array(18);
  out[0] = cmd & 0xff;
  out[1] = IDENTIFY_STAGE2_MARKER;
  out.set(out16, 2);
  return out;
}

function bleCommPackageForWrite(payloadBytes, txSeqState) {
  // Minimal mirror of BleCommProtocol.b()/d() for the Identify payloads.
  // Returns { frames: Uint8Array[], nextSeq: number }
  if (!(payloadBytes instanceof Uint8Array) || payloadBytes.byteLength < 1) {
    throw new Error("bleCommPackageForWrite expects non-empty payload");
  }

  let seq = txSeqState & 0xff;
  const frames = [];

  if (payloadBytes.byteLength <= 15) {
    const cmd = payloadBytes[0];
    const rest = payloadBytes.slice(1);
    // Matches BleCommProtocol.b(): lenByte = payloadLen + 2, totalLen = lenByte + 3.
    const lenByte = (payloadBytes.byteLength + 2) & 0xff;
    const frame = new Uint8Array(payloadBytes.byteLength + 5);
    frame[0] = 0xb0;
    frame[1] = lenByte;
    frame[2] = 0x00;
    frame[3] = seq;
    frame[4] = cmd;
    frame.set(rest, 5);
    let sum = 0;
    for (let i = 2; i < frame.length - 1; i++) {
      sum = (sum + frame[i]) & 0xff;
    }
    frame[frame.length - 1] = sum;
    frames.push(frame);
    seq = (seq + 2) & 0xff;
    return { frames, nextSeq: seq };
  }

  const cmd = payloadBytes[0];
  const rest = payloadBytes.slice(1);
  const restLen = rest.byteLength;
  const numFrags = Math.floor(restLen / 14) + 1;
  const lastIndex = numFrags - 1;

  // Chunk lens: 14 for all but last, remainder for last.
  const chunkLens = new Array(numFrags);
  for (let i = 0; i < numFrags - 1; i++) {
    chunkLens[i] = 14;
  }
  chunkLens[lastIndex] = restLen % 14;

  let off = 0;
  for (let fragIndex = 0; fragIndex < numFrags; fragIndex++) {
    const chunkLen = chunkLens[fragIndex];
    const frameLen = chunkLen + 6;
    const frame = new Uint8Array(frameLen);
    frame[0] = 0xb0;
    frame[1] = (chunkLen + 3) & 0xff;
    frame[2] = (((lastIndex << 4) + lastIndex - fragIndex) & 0xff);
    frame[3] = seq;
    frame[4] = cmd;
    frame.set(rest.slice(off, off + chunkLen), 5);
    off += chunkLen;
    let sum = 0;
    for (let i = 2; i < frame.length - 1; i++) {
      sum = (sum + frame[i]) & 0xff;
    }
    frame[chunkLen + 5] = sum;
    frames.push(frame);
    seq = (seq + 2) & 0xff;
  }

  return { frames, nextSeq: seq };
}

function buildVendorFragmentAck(inMeta, inSeq, cmd) {
  const ackCmd = (inMeta + 0x70) & 0xff;
  const ackSeq = (inSeq + 1) & 0xff;
  const checksum = (ackCmd + ackSeq + (cmd & 0xff)) & 0xff;
  return new Uint8Array([0xb0, 0x03, ackCmd, ackSeq, cmd & 0xff, checksum]);
}

function resetVendorRx() {
  vendorRx.buf = new Uint8Array(0);
  vendorRx.reassembly = null;
  vendorRx.identifyBuffered = null;
  vendorRx.identifyResolver = null;
}

function vendorRxAppend(u8) {
  if (!(u8 instanceof Uint8Array) || u8.byteLength === 0) {
    return;
  }
  vendorRx.buf = concatU8([vendorRx.buf, u8]);
}

function vendorRxParseFrames() {
  // Parses buffered A0 frames using the vendor rule: totalLen = frame[1] + 3.
  // Only processes fragmented frames whose frame[4] == 0xAC.
  while (vendorRx.buf.byteLength >= 2) {
    const start = vendorRx.buf.indexOf(0xa0);
    if (start === -1) {
      vendorRx.buf = new Uint8Array(0);
      return;
    }
    if (start > 0) {
      vendorRx.buf = vendorRx.buf.slice(start);
    }
    if (vendorRx.buf.byteLength < 2) {
      return;
    }
    const totalLen = (vendorRx.buf[1] & 0xff) + 3;
    if (totalLen < 6 || totalLen > 512) {
      // Drop one byte and resync.
      vendorRx.buf = vendorRx.buf.slice(1);
      continue;
    }
    if (vendorRx.buf.byteLength < totalLen) {
      return;
    }
    const frame = vendorRx.buf.slice(0, totalLen);
    vendorRx.buf = vendorRx.buf.slice(totalLen);

    // Validate checksum: sum(frame[2..last-1]) & 0xff == frame[last]
    let sum = 0;
    for (let i = 2; i < frame.length - 1; i++) {
      sum = (sum + frame[i]) & 0xff;
    }
    if (sum !== (frame[frame.length - 1] & 0xff)) {
      continue;
    }

    const meta = frame[2] & 0xff;
    // Mirror BleUnPackageData.unPackageData():
    // - meta == 0x00 => unfragmented message (e.g., A0 04 prompts) — do NOT ACK as fragments.
    // - meta == 0xF0 => special path (not part of Identify challenge)
    // - meta < 0xA0  => fragmented message
    if (meta === 0x00 || meta === 0xf0 || meta >= 0xa0) {
      continue;
    }
    if (frame.length < 7) {
      continue;
    }
    const cmd = frame[4] & 0xff;
    if (cmd !== IDENTIFY_CMD) {
      continue;
    }
    void handleVendorFragmentFrame(frame);
  }
}

async function sendVendorWrite(bytes, message) {
  const writer = handshake.writer;
  if (!writer || !(writer.properties.writeWithoutResponse || writer.properties.write)) {
    return;
  }
  const DIR_TX = "🟦C→P";
  if (message) {
    log(message, DIR_TX, bytes);
  }
  if (writer.properties.writeWithoutResponse) {
    await writer.writeValueWithoutResponse(bytes);
  } else {
    await writer.writeValue(bytes);
  }
}

async function handleVendorFragmentFrame(frame) {
  const meta = frame[2] & 0xff;
  const totalFragments = (meta >>> 4) + 1;
  const reverseIndex = meta & 0x0f;
  const seq = frame[3] & 0xff;
  const cmd = frame[4] & 0xff;

  const fragIndex = totalFragments - reverseIndex - 1;

  // Start (or restart) reassembly if this looks like a new sequence.
  if (!vendorRx.reassembly || vendorRx.reassembly.totalFragments !== totalFragments) {
    vendorRx.reassembly = {
      totalFragments,
      opcode: null,
      expectedSeq: null,
      parts: new Map(),
    };
  }

  if (!vendorRx.reassembly.expectedSeq) {
    const expectedSeq = new Array(totalFragments);
    for (let i = 0; i < totalFragments; i++) {
      expectedSeq[i] = (seq + (i - fragIndex) * 2 + 256 * 8) & 0xff;
    }
    vendorRx.reassembly.expectedSeq = expectedSeq;
  }

  let payload;
  if (reverseIndex === totalFragments - 1) {
    vendorRx.reassembly.opcode = frame[5] & 0xff;
    payload = frame.slice(6, frame.length - 1);
  } else {
    payload = frame.slice(5, frame.length - 1);
  }

  vendorRx.reassembly.parts.set(seq, payload);

  // ACK every fragment.
  const ack = buildVendorFragmentAck(meta, seq, cmd);
  try {
    await sendVendorWrite(ack, `Write fragment ACK meta=0x${meta.toString(16).padStart(2, "0")} seq=0x${seq.toString(16).padStart(2, "0")}`);
  } catch (e) {
    log(`Fragment ACK write failed: ${e?.message ?? e}`);
  }

  if (vendorRx.reassembly.parts.size !== totalFragments) {
    return;
  }

  const expected = vendorRx.reassembly.expectedSeq;
  const parts = [];
  for (const s of expected) {
    const p = vendorRx.reassembly.parts.get(s);
    if (!p) {
      return;
    }
    parts.push(p);
  }
  const assembled = concatU8(parts);
  const opcode = vendorRx.reassembly.opcode;
  vendorRx.reassembly = null;

  if (opcode !== null && assembled.byteLength === 48) {
    if (vendorRx.identifyResolver) {
      const r = vendorRx.identifyResolver;
      vendorRx.identifyResolver = null;
      r({ opcode, payload48: assembled });
    } else {
      vendorRx.identifyBuffered = { opcode, payload48: assembled };
    }
  } else {
    log(
      `Reassembled vendor opcode=0x${(opcode ?? 0).toString(16).padStart(2, "0")} len=${assembled.byteLength} (ignored)`
    );
  }
}

function waitForIdentifyChallenge(timeoutMs) {
  if (vendorRx.identifyBuffered) {
    const v = vendorRx.identifyBuffered;
    vendorRx.identifyBuffered = null;
    return Promise.resolve(v);
  }
  return new Promise((resolve, reject) => {
    let settled = false;
    const finishOk = (v) => {
      if (settled) return;
      settled = true;
      clearTimeout(t);
      resolve(v);
    };
    vendorRx.identifyResolver = finishOk;
    const t = setTimeout(() => {
      if (settled) return;
      settled = true;
      if (vendorRx.identifyResolver === finishOk) {
        vendorRx.identifyResolver = null;
      }
      reject(new Error("Timed out waiting for Identify challenge"));
    }, timeoutMs);
  });
}

function decodeVendorMeasurementFrame(u8) {
  if (!(u8 instanceof Uint8Array)) {
    throw new Error("Expected Uint8Array");
  }
  if (u8.byteLength !== 20) {
    throw new Error(`Expected 20 bytes, got ${u8.byteLength}`);
  }
  if (u8[0] !== 0xa0 || u8[1] !== 0x11) {
    throw new Error("Unexpected frame header");
  }

  const type = u8[2];

  const seq = u8[3];
  const sessionHex = toHex(u8.slice(4, 8));

  const strength = u8[8];
  // u8[9] is direct SpO2 percentage (confirmed from pcap2 — values 0x5b-0x64 = 91-100%).
  // Earlier code incorrectly added +50.
  const spo2 = u8[9];

  // bytes 10-11 are constant/config (usually 00 E8)
  const config = (u8[10] << 8) | u8[11];

  const hr10 = (u8[12] << 8) | u8[13];
  const hrBpm = hr10 / 10.0;

  const pleth = [
    (u8[14] << 8) | u8[15],
    (u8[16] << 8) | u8[17],
    (u8[18] << 8) | u8[19],
  ];

  const valid = !(strength === 0 || (pleth[0] === 0 && pleth[1] === 0 && pleth[2] === 0));

  return {
    type,
    seq,
    sessionHex,
    strength,
    spo2,
    config,
    hrBpm,
    pleth,
    valid,
  };
}

function looksLikeMeasurementFrame(decoded) {
  if (!decoded) {
    return false;
  }

  // Strong signal from the capture/README: measurement frames had config 0x00E8
  // and strength in 0..8.
  if (decoded.config !== 0x00e8) {
    return false;
  }
  if (!Number.isFinite(decoded.hrBpm) || decoded.hrBpm <= 0 || decoded.hrBpm > 250) {
    return false;
  }
  if (!Number.isFinite(decoded.spo2) || decoded.spo2 < 50 || decoded.spo2 > 110) {
    return false;
  }
  if (!Number.isInteger(decoded.strength) || decoded.strength < 0 || decoded.strength > 8) {
    return false;
  }
  return true;
}

async function stopAllNotifications() {
  const chars = activeNotificationChars;
  activeNotificationChars = [];

  await Promise.all(
    chars.map(async (c) => {
      try {
        c.removeEventListener("characteristicvaluechanged", onCharacteristicValueChanged);
      } catch {
        // ignore
      }
      try {
        await c.stopNotifications();
      } catch {
        // ignore
      }
    })
  );
}

function onDisconnected() {
  log("Disconnected");
  setStatus("Disconnected");

  vendorHandshakeComplete = false;

  gattServer = null;
  measurementCharacteristic = null;

  el.disconnectBtn.disabled = true;
  el.connectBtn.disabled = false;
  el.measurementChar.textContent = "—";
}

function onCharacteristicValueChanged(event) {
  const dv = event.target.value;
  const u8 = new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
  const DIR_RX = "🟩P→C";

  // During the early handshake, parse vendor A0 frames for the Identify
  // challenge fragments and immediately ACK them (B0 03 ...).
  if (handshake.running && !vendorHandshakeComplete) {
    vendorRxAppend(u8);
    vendorRxParseFrames();
  }

  // After the handshake completes we want visibility into *everything* the
  // device sends for a short window (some firmwares switch message types).
  if (!firstMeasurementLogged && Date.now() < postHandshakeDebugUntilMs) {
    log(`Post-handshake notification (len=${u8.byteLength})`, DIR_RX, u8);
  }

  const A011_MAGIC = new Uint8Array([0xa0, 0x11]);
  const a011Pos = findBytes(u8, A011_MAGIC);

  // If the device batches frames (larger MTU) we may see >20 byte notifications
  // that still contain one-or-more 20-byte A0 11 xx frames.
  if (a011Pos !== -1 && u8.byteLength !== 20 && nonFrameLogCount < NON_FRAME_LOG_LIMIT) {
    log(
      `Notification contains A0 11 at offset ${a011Pos} (len=${u8.byteLength}); scanning…`
    );
    nonFrameLogCount += 1;
  }

  // Helpful when reverse engineering: show a few notifications even if they
  // don't match the 20-byte measurement frame format.
  if (nonFrameLogCount < NON_FRAME_LOG_LIMIT) {
    log(`Notification (len=${u8.byteLength})`, DIR_RX, u8);
    nonFrameLogCount += 1;
  }

  // Some firmwares send additional status/data messages (not A0 11).
  // Always surface these so we don't miss alternative measurement formats.
  if (u8.byteLength >= 4 && u8[0] === 0xa0 && u8[1] === 0x0a) {
    log(`A0 0A message (len=${u8.byteLength})`, DIR_RX, u8);
  }

  // A0 06: "finger detected / sensor active" challenge message.
  // The pcap shows this 12-byte message at base=0x20 (stage-5 position) when a
  // finger was on the sensor during the handshake.  It REPLACES the normal
  // A0 04 prompt for that base, so we treat it the same way but mark it isA006
  // so the stage handler knows to use the capture-hardcoded B0 04 bytes (the
  // derivation formula differs from A0 04 and we only have one pcap sample).
  if (u8.byteLength === 12 && u8[0] === 0xa0 && u8[1] === 0x06 && u8[2] === 0x00 && u8[4] === 0xac) {
    const base = u8[3];
    log(
      `A0 06 finger-detected base=0x${base.toString(16).padStart(2, "0")}`,
      DIR_RX,
      u8
    );
    if (handshake.running) {
      const entry = { base, tail1: u8[5], tail2: u8[6], isA006: true };
      handshake.promptSeenByBase?.set?.(base, entry);
      if (handshake.expectedPromptBase !== null && base === handshake.expectedPromptBase) {
        const resolve = handshake.promptResolver;
        handshake.expectedPromptBase = null;
        handshake.promptResolver = null;
        handshake.promptSeenByBase?.delete?.(base);
        if (resolve) resolve(entry);
      }
    }
  }

  // Handle short ACKs used by the vendor handshake: a0 03 a0 <step> ...
  if (
    handshake.running &&
    u8.byteLength === 6 &&
    u8[0] === 0xa0 &&
    u8[1] === 0x03 &&
    u8[2] === 0xa0
  ) {
    const step = u8[3];
    handshake.ackSeenSteps?.add(step);
    if (handshake.expectedAckStep !== null && step === handshake.expectedAckStep) {
      // Consume this step from the buffer so future waits for the same step
      // (e.g. post-handshake sequences that wrap around) don't resolve
      // immediately using stale state.
      handshake.ackSeenSteps?.delete(step);
      log(`ACK step=0x${step.toString(16).padStart(2, "0")}`, DIR_RX, u8);
      const resolve = handshake.ackResolver;
      if (resolve) {
        resolve();
      }
    }
  }

  // Some firmwares emit a 7-byte prompt after handshake, e.g.
  //   a0 04 00 2e ac ff d9
  // This appears to request the *next* vendor stage.
  if (
    u8.byteLength === 7 &&
    u8[0] === 0xa0 &&
    u8[1] === 0x04 &&
    u8[2] === 0x00 &&
    u8[4] === 0xac
  ) {
    const base = u8[3];
    const t1 = u8[5];
    const t2 = u8[6];

    // Buffer prompts so handshake steps can wait for a specific base (even if
    // the notification arrives before the wait is established).
    if (handshake.running) {
      handshake.promptSeenByBase?.set?.(base, { base, tail1: t1, tail2: t2 });
      if (handshake.expectedPromptBase !== null && base === handshake.expectedPromptBase) {
        const resolve = handshake.promptResolver;
        handshake.expectedPromptBase = null;
        handshake.promptResolver = null;
        handshake.promptSeenByBase?.delete?.(base);
        if (resolve) {
          resolve({ base, tail1: t1, tail2: t2 });
        }
      }
    }

    // If the device is running a cyclic prompt sequence, the base will
    // eventually wrap (e.g. ... FA, 00, 06, ...). Our post-handshake logic
    // de-dupes by base to avoid responding to rapid repeats, so we must reset
    // that de-dupe state on wrap.
    if (postHandshakePrompt.lastSeenBase !== null) {
      const prev = postHandshakePrompt.lastSeenBase;
      if (base < prev) {
        postHandshakePrompt.handledBases.clear();
        postHandshakePrompt.enqueuedBases.clear();
        postHandshakePrompt.queue = [];
        log(
          `A0 04 prompt base wrapped (0x${prev
            .toString(16)
            .padStart(2, "0")} -> 0x${base
            .toString(16)
            .padStart(2, "0")}); resetting prompt de-dupe state`
        );
      }
    }
    postHandshakePrompt.lastSeenBase = base;

    // Wireshark-like RX row with the raw bytes.
    log(
      `A0 04 prompt base=0x${base.toString(16).padStart(2, "0")} tail=${t1
        .toString(16)
        .padStart(2, "0")}${t2.toString(16).padStart(2, "0")}`,
      DIR_RX,
      u8
    );

    // After the main staged handshake (ACK 0x2c), do NOT respond with any
    // further vendor writes. This matches the pcap behavior: the central is
    // done, and all subsequent traffic is peripheral->central.
    if (vendorHandshakeComplete && !POST_HANDSHAKE_WRITES_ENABLED && !firstMeasurementLogged) {
      if (!postHandshakePrompt.ignoredBases.has(base)) {
        postHandshakePrompt.ignoredBases.add(base);
        log(
          `Ignoring post-handshake A0 04 prompt (writes disabled) base=0x${base
            .toString(16)
            .padStart(2, "0")}`
        );
      }
    }
  }

  // Log a small sample of 20-byte vendor messages (A0 11 ?? ...), since some
  // devices emit a few non-measurement types (31/32/33) during handshake.
  if (u8.byteLength === 20 && u8[0] === 0xa0 && u8[1] === 0x11) {
    const t = u8[2];
    const key = t.toString(16).padStart(2, "0");
    const count = a011TypeLogCounts.get(key) ?? 0;
    if (count < A011_TYPE_LOG_LIMIT) {
      a011TypeLogCounts.set(key, count + 1);
      log(`A0 11 type=0x${key} ascii=${toAsciiPreview(u8.slice(3))}`, DIR_RX, u8);
    }
  }

  // Extract and decode any embedded 20-byte measurement frames.
  // This supports both the classic 20-byte notification case and
  // MTU-enabled batching (40/60/... bytes).
  let foundMeasurement = false;
  for (let scanFrom = 0; scanFrom <= u8.byteLength - 3; ) {
    const pos = findBytes(u8, A011_MAGIC, scanFrom);
    if (pos === -1) {
      break;
    }
    if (pos + 20 <= u8.byteLength) {
      const frame = u8.slice(pos, pos + 20);
      if (frame[0] === 0xa0 && frame[1] === 0x11) {
        const rawHex = toHex(frame);
        if (rawHex !== lastRawHex) {
          lastRawHex = rawHex;

          let decoded;
          try {
            decoded = decodeVendorMeasurementFrame(frame);
          } catch (e) {
            log(`Frame decode error: ${e?.message ?? e}`);
            scanFrom = pos + 1;
            continue;
          }

          // Primary path: A0 11 F0 (per README/pcap)
          // Fallback: accept other A0 11 types only if they look exactly like
          // a measurement payload.
          if (decoded.type === 0xf0 || looksLikeMeasurementFrame(decoded)) {
            foundMeasurement = true;
            const typeHex = decoded.type.toString(16).padStart(2, "0");
            if (!firstMeasurementLogged) {
              firstMeasurementLogged = true;
              log(`Measurement frames started (A0 11 0x${typeHex} …)`);
              setStatus("Streaming measurements…");
            }

            lastFrameAtMs = Date.now();

            if (!measurementCharacteristic) {
              measurementCharacteristic = event.target;
              el.measurementChar.textContent = measurementCharacteristic.uuid;
              log(`Selected measurement characteristic: ${measurementCharacteristic.uuid}`);

          // Stop notifications on other characteristics to reduce noise.
          const keep = measurementCharacteristic;
          const toStop = activeNotificationChars.filter((c) => c !== keep);
          activeNotificationChars = [keep];
          void (async () => {
            await Promise.all(
              toStop.map(async (c) => {
                try {
                  c.removeEventListener(
                    "characteristicvaluechanged",
                    onCharacteristicValueChanged
                  );
                } catch {}
                try {
                  await c.stopNotifications();
                } catch {}
              })
            );
          })();
        }

            el.spo2.textContent = `${decoded.spo2}%`;
            el.hr.textContent = `${decoded.hrBpm.toFixed(1)} bpm`;
            el.strength.textContent = `${decoded.strength}/8`;
            el.pleth.textContent = `[${decoded.pleth.join(", ")}]`;
            el.frameMeta.textContent = `type=0x${typeHex} seq=${decoded.seq} session=${decoded.sessionHex} config=0x${decoded.config
              .toString(16)
              .padStart(4, "0")} ${decoded.valid ? "valid" : "weak"}`;
            el.rawHex.textContent = rawHex;
          }
        }
      }
    }
    scanFrom = pos + 1;
  }

  if (foundMeasurement) {
    return;
  }
}

function buildB003(step) {
  // Observed pattern in the pcap: b0 03 a0 <step> ac <step+0x4c>
  const last = (step + 0x4c) & 0xff;
  return new Uint8Array([0xb0, 0x03, 0xa0, step & 0xff, 0xac, last]);
}

function buildB004Fixed(step, dataByte) {
  // Format: b0 04 00 <step> ac <data> <checksum>
  // checksum == (sum(frame[2..len-2]) & 0xff)
  const s = step & 0xff;
  const d = dataByte & 0xff;
  const checksum = (0x00 + s + 0xac + d) & 0xff;
  return new Uint8Array([0xb0, 0x04, 0x00, s, 0xac, d, checksum]);
}

function buildB004FromPrompt(step, tail1, tail2) {
  // For prompt-driven B0 04 writes, the device provides a 2-byte tail (t1,t2)
  // where we observed:
  //   dataByte = t1
  //   checksum == (t2 + 3) & 0xff
  // and checksum must also equal (0x00 + step + 0xac + dataByte) & 0xff.
  const s = step & 0xff;
  const d = tail1 & 0xff;
  const checksum = (0x00 + s + 0xac + d) & 0xff;
  const expected = (tail2 + 3) & 0xff;
  if (expected !== checksum) {
    log(
      `B0 04 checksum mismatch for step=0x${s.toString(16).padStart(2, "0")}: ` +
      `prompt tail suggests 0x${expected.toString(16).padStart(2, "0")}, computed 0x${checksum
        .toString(16)
        .padStart(2, "0")}`
    );
  }
  return new Uint8Array([0xb0, 0x04, 0x00, s, 0xac, d, checksum]);
}

function enqueuePostHandshakePrompt(base, tail1, tail2) {
  if (!POST_HANDSHAKE_WRITES_ENABLED) {
    return;
  }
  if (postHandshakePrompt.handledBases.has(base)) {
    return;
  }
  if (postHandshakePrompt.enqueuedBases.has(base)) {
    return;
  }
  postHandshakePrompt.enqueuedBases.add(base);
  postHandshakePrompt.queue.push({ base, tail1, tail2 });
  void drainPostHandshakePromptQueue();
}

async function drainPostHandshakePromptQueue() {
  if (!POST_HANDSHAKE_WRITES_ENABLED) {
    return;
  }
  if (postHandshakePrompt.inFlight) {
    return;
  }

  postHandshakePrompt.inFlight = true;
  try {
    // Process prompts sequentially. If another prompt arrives mid-flight, it
    // will be enqueued and picked up by this loop (or the next drain).
    while (postHandshakePrompt.queue.length) {
      const next = postHandshakePrompt.queue.shift();
      if (!next) {
        break;
      }
      postHandshakePrompt.enqueuedBases.delete(next.base);

      // Stop if we disconnected.
      if (!device?.gatt?.connected) {
        return;
      }

      await respondToPostHandshakePrompt(next.base, next.tail1, next.tail2);
    }
  } finally {
    postHandshakePrompt.inFlight = false;
  }
}

async function respondToPostHandshakePrompt(base, tail1, tail2) {
  if (!POST_HANDSHAKE_WRITES_ENABLED) {
    return;
  }
  // We send B0 03 + B0 04 with dynamic tails from the A0 04 prompt.
  //
  // Reference note: pulse-ox-3 shows B0 03 only during its idle loop (base=0x94+),
  // but that capture starts with the finger already on the sensor — the idle loop
  // occurs *after* a running session (finger removed). We have no reference for
  // base=0x2e immediately after handshake with no finger.
  //
  // Live device evidence:
  //   B0 03 only          → device re-prompts once then goes completely silent
  //   B0 03 + B0 04 (dyn) → device keeps cycling (stays alive for finger detection)
  const writer = handshake.writer;
  if (!writer || !(writer.properties.writeWithoutResponse || writer.properties.write)) {
    return;
  }
  if (!vendorHandshakeComplete || firstMeasurementLogged) {
    return;
  }
  if (!device?.gatt?.connected) {
    return;
  }

  const stepB003 = (base + 1) & 0xff;
  const stepB004 = (base + 3) & 0xff;
  const w1 = buildB003(stepB003);
  const w2 = buildB004FromPrompt(stepB004, tail1, tail2);

  const DIR_TX = "🟦C→P";
  log(
    `Write B0 03 (post-handshake) base=0x${base.toString(16).padStart(2, "0")}`,
    DIR_TX,
    w1
  );
  log(
    `Write B0 04 (post-handshake) step=0x${stepB004.toString(16).padStart(2, "0")}`,
    DIR_TX,
    w2
  );

  try {
    if (writer.properties.writeWithoutResponse) {
      await writer.writeValueWithoutResponse(w1);
      await new Promise((r) => setTimeout(r, 20));
      await writer.writeValueWithoutResponse(w2);
    } else {
      await writer.writeValue(w1);
      await new Promise((r) => setTimeout(r, 20));
      await writer.writeValue(w2);
    }
  } catch (e) {
    log(`Post-handshake write failed: ${e?.message ?? e}`);
  }

  postHandshakePrompt.handledBases.add(base);
}

async function connect() {
  if (!navigator.bluetooth) {
    throw new Error(
      "Web Bluetooth is not available. Use Chrome/Edge and a secure context (HTTPS or http://localhost)."
    );
  }

  setStatus("Requesting device…");

  vendorHandshakeComplete = false;

  // Restrict the chooser so only the pulse ox (or very close matches) appear.
  // Note: if the device doesn't advertise its vendor service UUID or name at
  // that moment, it may not show up. If that happens, temporarily switch back
  // to acceptAllDevices: true for debugging.
  device = await navigator.bluetooth.requestDevice({
    filters: [
      { services: [VENDOR_SERVICE_UUID_WIRESHARK] },
      { services: [VENDOR_SERVICE_UUID] },
      { services: [VENDOR_SERVICE_UUID_ALT] },
      { namePrefix: "Pulse" },
      { namePrefix: "Oximeter" },
    ],
    optionalServices: [
      VENDOR_SERVICE_UUID,
      VENDOR_SERVICE_UUID_ALT,
      VENDOR_SERVICE_UUID_WIRESHARK,
      GENERIC_ACCESS_UUID,
      GENERIC_ATTRIBUTE_UUID,
      "battery_service",
      "device_information",
    ],
  });

  el.device.textContent = device.name || device.id;
  el.serviceUuid.textContent = VENDOR_SERVICE_UUID;

  device.addEventListener("gattserverdisconnected", onDisconnected);

  setStatus("Connecting…");
  log(`Connecting to ${device.name || device.id}`);

  gattServer = await device.gatt.connect();

  setStatus("Discovering services…");
  let allServices = [];
  try {
    allServices = await gattServer.getPrimaryServices();
    if (allServices.length) {
      log(
        `Accessible primary services (${allServices.length}): ${allServices
          .map((s) => s.uuid)
          .join(", ")}`
      );
    }
  } catch (e) {
    log(`getPrimaryServices() failed: ${e?.message ?? e}`);
  }

  setStatus("Discovering vendor service…");
  const serviceUuidsToTry = [
    VENDOR_SERVICE_UUID,
    VENDOR_SERVICE_UUID_ALT,
    VENDOR_SERVICE_UUID_WIRESHARK,
  ];

  let service = null;
  let activeServiceUuid = null;

  // Some devices need a beat after connect before their custom services become
  // discoverable via CoreBluetooth/Web Bluetooth. Retry with a small backoff.
  for (let attempt = 1; attempt <= 4 && !service; attempt++) {
    for (const candidateUuid of serviceUuidsToTry) {
      try {
        service = await gattServer.getPrimaryService(candidateUuid);
        activeServiceUuid = candidateUuid;
        break;
      } catch (e) {
        log(
          `Vendor service not found (attempt ${attempt}/4, uuid ${candidateUuid}): ${
            e?.message ?? e
          }`
        );
      }
    }

    if (!service) {
      await new Promise((r) => setTimeout(r, 250 * attempt));
    }
  }

  if (!service) {
    const msg =
      "Selected device does not expose the pulse-ox vendor service. " +
      "If you're sure you selected the right device, try power-cycling the pulse ox and ensure it's disconnected from your phone app.";
    throw new Error(msg);
  }

  el.serviceUuid.textContent = activeServiceUuid;

  setStatus("Discovering characteristics…");
  const characteristics = await service.getCharacteristics();
  log(`Found ${characteristics.length} characteristic(s) under vendor service`);

  for (const c of characteristics) {
    const p = c.properties;
    const props = [
      p.read ? "read" : null,
      p.write ? "write" : null,
      p.writeWithoutResponse ? "writeWithoutResponse" : null,
      p.notify ? "notify" : null,
      p.indicate ? "indicate" : null,
    ].filter(Boolean);
    log(`Characteristic: ${c.uuid} [${props.join(", ")}]`);
  }

  const notifyCandidates = characteristics.filter(
    (c) => c.properties.notify || c.properties.indicate
  );
  if (notifyCandidates.length === 0) {
    throw new Error(
      "No notifying characteristics found under vendor service (nothing to subscribe to)."
    );
  }

  setStatus("Subscribing…");
  await stopAllNotifications();

  // Start notifications first (matches ordering observed in the pcap: CCCD enable
  // before the vendor command/ACK exchange).
  for (const c of notifyCandidates) {
    try {
      await c.startNotifications();
      c.addEventListener(
        "characteristicvaluechanged",
        onCharacteristicValueChanged
      );
      activeNotificationChars.push(c);
      log(`Notifications started: ${c.uuid}`);
    } catch (e) {
      log(`Failed to start notifications on ${c.uuid}: ${e?.message ?? e}`);
    }
  }

  el.disconnectBtn.disabled = false;
  el.connectBtn.disabled = true;

  setStatus("Connected — waiting for frames…");

  // Start the vendor handshake (ACK-driven) if the characteristic is writable.
  const notifyWritable = notifyCandidates.filter(
    (c) => c.properties.writeWithoutResponse || c.properties.write
  );
  const otherWritable = characteristics.filter(
    (c) => c.properties.writeWithoutResponse || c.properties.write
  );
  const writeCandidates = notifyWritable.length ? notifyWritable : otherWritable;

  if (writeCandidates.length) {
    const writer =
      writeCandidates.find((c) => c.properties.writeWithoutResponse) ??
      writeCandidates[0];
    resetVendorRx();
    handshake = {
      running: true,
      writer,
      txSeq: 1,
      expectedAckStep: null,
      ackResolver: null,
      ackSeenSteps: new Set(),
      expectedPromptBase: null,
      promptResolver: null,
      promptSeenByBase: new Map(),
      cancelToken: handshake.cancelToken + 1,
      stage5WasA006: false,
    };
    lastFrameAtMs = 0;
    firstMeasurementLogged = false;
    a011TypeLogCounts.clear();
    void runVendorHandshake(handshake.cancelToken);

    // Watchdog: if handshake finishes but no measurements start, hint at the
    // most common real-world cause (device not actively measuring).
    setTimeout(() => {
      if (!device?.gatt?.connected) {
        return;
      }
      if (firstMeasurementLogged) {
        return;
      }
      log(
        "No measurement frames yet. Make sure your finger is firmly on the sensor. " +
        "The device streams A0 11 F0 data only while a finger is detected — the A0 04 prompt loop you see is the idle/no-finger state."
      );
    }, 4000);
  } else {
    log("No writable characteristic found; cannot start vendor handshake");
  }
}

function waitForHandshakeAck(step, timeoutMs) {
  if (!handshake.running) {
    return Promise.reject(new Error("Handshake not running"));
  }

  // ACKs can arrive before the stage begins waiting (e.g., while we are still
  // sending the stage writes). Keep a small in-memory buffer so we can't miss
  // the transition.
  if (handshake.ackSeenSteps?.has(step)) {
    handshake.ackSeenSteps.delete(step);
    return Promise.resolve();
  }

  handshake.expectedAckStep = step;
  return new Promise((resolve, reject) => {
    let settled = false;

    const finishOk = () => {
      if (settled) {
        return;
      }
      settled = true;
      if (handshake.ackResolver === finishOk) {
        handshake.ackResolver = null;
      }
      if (handshake.expectedAckStep === step) {
        handshake.expectedAckStep = null;
      }
      clearTimeout(t);
      resolve();
    };

    handshake.ackResolver = finishOk;
    const t = setTimeout(() => {
      if (settled) {
        return;
      }
      settled = true;
      if (handshake.ackResolver === finishOk) {
        handshake.ackResolver = null;
      }
      if (handshake.expectedAckStep === step) {
        handshake.expectedAckStep = null;
      }
      reject(new Error(`Timed out waiting for ACK step 0x${step.toString(16)}`));
    }, timeoutMs);

    // Ensure timeout does not keep event loop alive unnecessarily.
    // (Browsers may ignore unref, but this is harmless.)
    if (typeof t === "object" && typeof t.unref === "function") {
      t.unref();
    }
  });
}

async function waitForHandshakeAckAny(steps, timeoutMs) {
  if (!handshake.running) {
    throw new Error("Handshake not running");
  }
  const start = performance.now();
  while (true) {
    for (const step of steps) {
      if (handshake.ackSeenSteps?.has?.(step)) {
        handshake.ackSeenSteps.delete(step);
        return step;
      }
    }
    if (performance.now() - start > timeoutMs) {
      const wanted = steps.map((s) => `0x${s.toString(16).padStart(2, "0")}`).join(", ");
      throw new Error(`Timed out waiting for ACK step (${wanted})`);
    }
    await new Promise((r) => setTimeout(r, 20));
  }
}

function waitForVendorPromptBase(base, timeoutMs) {
  if (!handshake.running) {
    return Promise.reject(new Error("Handshake not running"));
  }

  const buffered = handshake.promptSeenByBase?.get?.(base);
  if (buffered) {
    handshake.promptSeenByBase.delete(base);
    return Promise.resolve(buffered);
  }

  handshake.expectedPromptBase = base;
  return new Promise((resolve, reject) => {
    let settled = false;

    const finishOk = (prompt) => {
      if (settled) {
        return;
      }
      settled = true;
      if (handshake.promptResolver === finishOk) {
        handshake.promptResolver = null;
      }
      if (handshake.expectedPromptBase === base) {
        handshake.expectedPromptBase = null;
      }
      clearTimeout(t);
      resolve(prompt);
    };

    handshake.promptResolver = finishOk;
    const t = setTimeout(() => {
      if (settled) {
        return;
      }
      settled = true;
      if (handshake.promptResolver === finishOk) {
        handshake.promptResolver = null;
      }
      if (handshake.expectedPromptBase === base) {
        handshake.expectedPromptBase = null;
      }
      reject(new Error(`Timed out waiting for prompt base 0x${base.toString(16)}`));
    }, timeoutMs);

    if (typeof t === "object" && typeof t.unref === "function") {
      t.unref();
    }
  });
}

async function runVendorHandshake(cancelToken) {
  if (!handshake.writer) {
    return;
  }

  const writer = handshake.writer;
  try {
    // --- Stage 1: IdentifyIns.identify(0xAC) ---
    if (!device?.gatt?.connected || handshake.cancelToken !== cancelToken) {
      return;
    }
    setStatus("Vendor handshake (stage1)…");
    log(`Sending stage1 writes via ${writer.uuid}`);

    // Begin waiting before sending writes.
    const ackStage1 = waitForHandshakeAck(0x04, 5000);

    const identify1 = identifyInsIdentify(IDENTIFY_CMD);
    const packaged1 = bleCommPackageForWrite(identify1, handshake.txSeq);
    handshake.txSeq = packaged1.nextSeq;

    for (const frame of packaged1.frames) {
      await sendVendorWrite(frame, "Handshake stage1 write");
      await new Promise((r) => setTimeout(r, 60));
    }
    await ackStage1;

    // --- Stage 2: respond to device Identify challenge (opcode 0xFB, 48 bytes) ---
    if (!device?.gatt?.connected || handshake.cancelToken !== cancelToken) {
      return;
    }
    setStatus("Vendor handshake (stage2)…");
    log("Waiting for Identify challenge fragments…");
    const { opcode, payload48 } = await waitForIdentifyChallenge(5000);
    log(
      `Identify challenge received opcode=0x${opcode
        .toString(16)
        .padStart(2, "0")} (48 bytes): ${toHex(payload48)}`
    );

    const identify2 = identifyInsDeciphering(payload48, IDENTIFY_SELECTOR, IDENTIFY_CMD);
    const packaged2 = bleCommPackageForWrite(identify2, handshake.txSeq);
    handshake.txSeq = packaged2.nextSeq;

    for (const frame of packaged2.frames) {
      await sendVendorWrite(frame, "Handshake stage2 write");
      await new Promise((r) => setTimeout(r, 60));
    }

    // Firmware variance: pcap baseline ACKs stage2 at 0x12, but some devices
    // ACK stage2 at 0x08 (observed live). Accept either and derive an offset
    // for subsequent stage step numbers.
    const ackStage2Step = await waitForHandshakeAckAny([0x08, 0x12], 5000);
    log(
      `Stage2 complete (ACK step=0x${ackStage2Step
        .toString(16)
        .padStart(2, "0")}); deriving stages 3–6…`
    );

    // pcap baseline: stage2 ACK=0x12.
    // stage3 ACK=0x18, stage4 ACK=0x1e, stage5 ACK=0x24, stage6 ACK=0x2c.
    let delta = (ackStage2Step - 0x12) & 0xff;
    if (delta > 0x7f) {
      delta -= 0x100;
    }
    if (delta !== 0) {
      log(`Handshake step offset detected: ${delta} (relative to pcap baseline)`);
    }

    const stages = [
      {
        name: "stage3",
        completeAckStep: (0x18 + delta) & 0xff,
        promptBase: (0x14 + delta) & 0xff,
        b004Data: 0xc1,
        allowPrompt: true,
      },
      {
        name: "stage4",
        completeAckStep: (0x1e + delta) & 0xff,
        // Some firmwares emit A0 05 at this position (not buffered), so we
        // treat the prompt as optional.
        promptBase: (0x1a + delta) & 0xff,
        b004Data: 0xc6,
        allowPrompt: false,
      },
      {
        name: "stage5",
        completeAckStep: (0x24 + delta) & 0xff,
        promptBase: (0x20 + delta) & 0xff,
        b004Data: 0xa5,
        allowPrompt: true,
      },
      {
        name: "stage6",
        completeAckStep: (0x2c + delta) & 0xff,
        b004Data: 0xa6,
      },
    ];

    for (const stage of stages) {
      if (!device?.gatt?.connected || handshake.cancelToken !== cancelToken) {
        return;
      }

      setStatus(`Vendor handshake (${stage.name})…`);
      log(`Sending ${stage.name} writes via ${writer.uuid}`);

      const DIR_TX = "🟦C→P";

      // Important: begin waiting for the stage ACK *before* sending writes.
      // The device can ACK quickly and we would otherwise miss it.
      const ackPromise = waitForHandshakeAck(stage.completeAckStep, 5000);

      if (stage.name !== "stage6") {
        // --- Stages 3–5 ---
        // B0 03 step = ACK-3
        // B0 04 step = ACK-1
        // (Optional) A0 04 prompt base = ACK-4
        const b003Step = (stage.completeAckStep - 3) & 0xff;
        const b004Step = (stage.completeAckStep - 1) & 0xff;

        let stagePrompt = null;
        if (stage.allowPrompt && stage.promptBase !== undefined) {
          try {
            stagePrompt = await waitForVendorPromptBase(stage.promptBase, 400);
            log(
              `Handshake ${stage.name}: got prompt base=0x${stage.promptBase
                .toString(16)
                .padStart(2, "0")} tail=${stagePrompt.tail1
                .toString(16)
                .padStart(2, "0")}${stagePrompt.tail2.toString(16).padStart(2, "0")}`
            );
          } catch {
            // Not fatal.
          }
        }

        const w1 = buildB003(b003Step);
        log(`Handshake ${stage.name} write`, DIR_TX, w1);
        if (writer.properties.writeWithoutResponse) {
          await writer.writeValueWithoutResponse(w1);
        } else {
          await writer.writeValue(w1);
        }
        await new Promise((r) => setTimeout(r, 60));

        let w2;
        if (stage.name === "stage5" && stagePrompt && !stagePrompt.isA006) {
          // Stage5 can be prompt-derived on some firmwares.
          w2 = buildB004FromPrompt(b004Step, stagePrompt.tail1, stagePrompt.tail2);
          log(`Handshake ${stage.name} write (prompt-derived)`, DIR_TX, w2);
        } else {
          if (stage.name === "stage5") {
            handshake.stage5WasA006 = stagePrompt?.isA006 === true;
          }
          w2 = buildB004Fixed(b004Step, stage.b004Data);
          log(`Handshake ${stage.name} write`, DIR_TX, w2);
        }
        if (writer.properties.writeWithoutResponse) {
          await writer.writeValueWithoutResponse(w2);
        } else {
          await writer.writeValue(w2);
        }
      } else if (stage.name === "stage6") {
        // Capture shows:
        //   a004 base=0x26 ...
        //   b003 step=0x27 ...
        //   a004 base=0x28 ...
        //   b003 step=0x29 ...
        //   b004 step=0x2b tail=(from base=0x28)
        // Some firmwares never emit the base=0x28 prompt; in that case we
        // fall back to base=0x26 tails directly (same formula — buildB004
        // applies the +3 to tail2).

        const promptBase26 = (stage.completeAckStep - 6) & 0xff;
        const promptBase28 = (stage.completeAckStep - 4) & 0xff;

        let prompt26 = null;
        try {
          prompt26 = await waitForVendorPromptBase(promptBase26, 800);
          log(
            `Handshake ${stage.name}: got prompt base=0x${promptBase26
              .toString(16)
              .padStart(2, "0")} tail=${prompt26.tail1
              .toString(16)
              .padStart(2, "0")}${prompt26.tail2.toString(16).padStart(2, "0")}`
          );
        } catch {
          // Not fatal.
        }

        const first = buildB003((stage.completeAckStep - 5) & 0xff);
        log(`Handshake ${stage.name} write`, DIR_TX, first);
        if (writer.properties.writeWithoutResponse) {
          await writer.writeValueWithoutResponse(first);
        } else {
          await writer.writeValue(first);
        }
        await new Promise((r) => setTimeout(r, 60));

        let prompt28 = null;
        try {
          prompt28 = await waitForVendorPromptBase(promptBase28, 200);
          log(
            `Handshake ${stage.name}: got prompt base=0x${promptBase28
              .toString(16)
              .padStart(2, "0")} tail=${prompt28.tail1
              .toString(16)
              .padStart(2, "0")}${prompt28.tail2.toString(16).padStart(2, "0")}`
          );
        } catch (e) {
          log(
            `Handshake ${stage.name}: did not see prompt base=0x${promptBase28
              .toString(16)
              .padStart(2, "0")} (${e?.message ?? e}); using base=0x${promptBase26
              .toString(16)
              .padStart(2, "0")} tail if available`
          );
        }

        const dataByteForB004 = prompt28?.tail1 ?? stage.b004Data;
        if (!prompt28) {
          log(
            `Handshake ${stage.name}: base=0x${promptBase28
              .toString(16)
              .padStart(2, "0")} not received — using fixed B0 04 data byte 0x${stage.b004Data
              .toString(16)
              .padStart(2, "0")}`
          );
        }

        const w1 = buildB003((stage.completeAckStep - 3) & 0xff);
        const w2 = buildB004Fixed((stage.completeAckStep - 1) & 0xff, dataByteForB004);
        log(`Handshake ${stage.name} write`, DIR_TX, w1);
        log(`Handshake ${stage.name} write`, DIR_TX, w2);
        if (writer.properties.writeWithoutResponse) {
          await writer.writeValueWithoutResponse(w1);
          await new Promise((r) => setTimeout(r, 60));
          await writer.writeValueWithoutResponse(w2);
        } else {
          await writer.writeValue(w1);
          await new Promise((r) => setTimeout(r, 60));
          await writer.writeValue(w2);
        }
      } else {
        for (const hex of stage.writesHex) {
          const bytes = fromHex(hex);
          log(`Handshake ${stage.name} write`, DIR_TX, bytes);
          if (writer.properties.writeWithoutResponse) {
            await writer.writeValueWithoutResponse(bytes);
          } else {
            await writer.writeValue(bytes);
          }
          await new Promise((r) => setTimeout(r, 60));
        }
      }

      await ackPromise;
    }

    handshake.running = false;
    handshake.expectedAckStep = null;
    handshake.ackResolver = null;
    vendorHandshakeComplete = true;
    setStatus("Handshake complete — waiting for measurements");
    log("Vendor handshake complete");
    if (handshake.stage5WasA006) {
      log("✅ Stage 5: A0 06 finger-detected was seen");
    }

    // Post-handshake central writes are disabled (see POST_HANDSHAKE_WRITES_ENABLED).
    postHandshakePrompt.queue = [];
    postHandshakePrompt.enqueuedBases.clear();
    postHandshakePrompt.handledBases.clear();
    postHandshakePrompt.respondAfterMs = 0;
    postHandshakePrompt.deferNoticeLogged = false;

    // Reset limited logging and enable a short full-hex debug window so we can
    // see what the device does immediately after handshake completion.
    nonFrameLogCount = 0;
    a011TypeLogCounts.clear();
    postHandshakeDebugUntilMs = Date.now() + 10_000;

    // The staged handshake is complete; clear any previously-seen ACK steps so
    // post-handshake waits can't be satisfied by earlier handshake ACKs.
    handshake.ackSeenSteps?.clear?.();
    handshake.promptSeenByBase?.clear?.();
  } catch (e) {
    handshake.running = false;
    handshake.expectedAckStep = null;
    handshake.ackResolver = null;
    vendorHandshakeComplete = false;
    log(`Vendor handshake failed: ${e?.message ?? e}`);
    setStatus("Handshake failed (see log)");
  }
}

async function disconnect() {
  el.disconnectBtn.disabled = true;
  vendorHandshakeComplete = false;
  try {
    await stopAllNotifications();
  } catch {
    // ignore
  }

  if (device?.gatt?.connected) {
    setStatus("Disconnecting…");
    device.gatt.disconnect();
  }
}

el.connectBtn.addEventListener("click", async () => {
  el.connectBtn.disabled = true;
  resetLog();
  lastRawHex = null;
  nonFrameLogCount = 0;
  handshake = {
    running: false,
    writer: null,
    expectedAckStep: null,
    ackResolver: null,
    ackSeenSteps: new Set(),
    expectedPromptBase: null,
    promptResolver: null,
    promptSeenByBase: new Map(),
    cancelToken: handshake.cancelToken + 1,
    stage5WasA006: false,
  };
  vendorHandshakeComplete = false;
  postHandshakePrompt = {
    inFlight: false,
    queue: [],
    enqueuedBases: new Set(),
    handledBases: new Set(),
    lastSeenBase: null,
    respondAfterMs: 0,
    deferNoticeLogged: false,
    ignoredBases: new Set(),
  };
  firstMeasurementLogged = false;
  a011TypeLogCounts.clear();

  try {
    await connect();
  } catch (e) {
    log(`Connect error: ${e?.message ?? e}`);
    setStatus("Error (see log)");
    el.connectBtn.disabled = false;
  }
});

el.disconnectBtn.addEventListener("click", async () => {
  try {
    await disconnect();
  } catch (e) {
    log(`Disconnect error: ${e?.message ?? e}`);
  }
});

el.copyLogBtn?.addEventListener("click", async () => {
  try {
    await copyDebugLogToClipboard();
  } catch (e) {
    // Don't alert; keep UX minimal.
    console.error("Copy log failed", e);
  }
});

setStatus("Idle");
el.serviceUuid.textContent = VENDOR_SERVICE_UUID;
resetLog();
