const APP_VERSION = "2026-03-14.4";

// Extracted from data/gk-plus-1.pcapng via tshark-in-Docker.
// Note: 128-bit UUIDs are transmitted little-endian over the air; the canonical
// UUID strings below are the correctly formatted values from service discovery.
const GK_SERVICE_UUID_PCAP = "0003cdd0-0000-1000-8000-00805f9b0131";
const GK_NOTIFY_CHAR_UUID_PCAP = "0003cdd1-0000-1000-8000-00805f9b0131";
const GK_WRITE_CHAR_UUID_PCAP = "0003cdd2-0000-1000-8000-00805f9b0131";

// Older hypothesis (kept as optional fallback):
const GK_SERVICE_UUID = "31019b5f-8000-0080-0010-0000d0cd0300";
const GK_NOTIFY_CHAR_UUID = "31019b5f-8000-0080-0010-0000d1cd0300"; // handle 0x001f
const GK_WRITE_CHAR_UUID = "31019b5f-8000-0080-0010-0000d2cd0300"; // handle 0x0022

// The meter may not advertise the full 128-bit UUID(s) in its advertising data.
// In the pcaps, the vendor service is also visible as 16-bit UUID 0xFFF0.
// Using this as an optional service allows access after connection.
const GK_SERVICE_UUID_16 = "0000fff0-0000-1000-8000-00805f9b34fb";

// Advertising (pcaps): the meter does not include a Local Name and does not
// advertise 0xFFF0, but it consistently includes Service Data (AD type 0x16)
// with 16-bit UUID 0x78ac.
// Web Bluetooth can only filter the chooser on *advertised services*; in
// Chromium, filtering by this UUID is the tightest observed option.
const GK_ADV_SERVICE_UUID_16 = 0x78ac;

// Observed in Chrome device picker: "Keto-Mojo GK+ Meter - Paired".
// The suffix is UI/OS-specific; filtering by a short prefix is more robust.
const GK_DEVICE_NAME_PREFIX = "Keto-Mojo";

// Requests observed in both pcaps (writes on handle 0x0022).
const REQ_INFO_66 = fromHex("7b0110012066550000010e08087d");
const REQ_INIT_AA = fromHex("7b01100120aa55000002010d087d");
const REQ_INFO_77 = fromHex("7b0110012077550000010b0b047d");
// APK-derived: 0xdb is used for *records count* (not glucose/ketone values).
const REQ_RECORDS_COUNT_DB = fromHex("7b01100120db550000030a0e047d");
// APK-derived: request record transfer (history/latest records). This command is
// parameterized and includes CRC bytes (nibble-expanded) before the trailing 0x7d.
// See com/ketomojo/sdk/device/vivachek/LatestRecordsCommandFactory.generateCommand(I).
const REQ_RECORDS_16_BODY_PREFIX_HEX = "0110012016550002";

// Observed in pcaps: set-time frame shape is constant except the 6-byte time.
// We do not yet know what the 4 trailing bytes represent; we keep the same
// suffix bytes seen in gk-plus-1 as a best-effort attempt.
const REQ_SET_TIME_44_SUFFIX = fromHex("0c000a0b");

const el = {
  connectBtn: document.getElementById("connectBtn"),
  disconnectBtn: document.getElementById("disconnectBtn"),
  fetchBtn: document.getElementById("fetchBtn"),
  recordsToFetch: document.getElementById("recordsToFetch"),
  copyLogBtn: document.getElementById("copyLogBtn"),
  copySampleBtn: document.getElementById("copySampleBtn"),
  copyReadingsBtn: document.getElementById("copyReadingsBtn"),
  status: document.getElementById("status"),
  setTime: document.getElementById("setTime"),
  device: document.getElementById("device"),
  serviceUuid: document.getElementById("serviceUuid"),
  chars: document.getElementById("chars"),
  glucose: document.getElementById("glucose"),
  ketone: document.getElementById("ketone"),
  gki: document.getElementById("gki"),
  mealTag: document.getElementById("mealTag"),
  mealIcon: document.getElementById("mealIcon"),
  mealText: document.getElementById("mealText"),
  recordId: document.getElementById("recordId"),
  lastRx: document.getElementById("lastRx"),
  log: document.getElementById("log"),
};

let device = null;
let gattServer = null;
let notifyChar = null;
let writeChar = null;
let writeCharAlt = null;
let activeServiceUuid = null;

let subscribedNotifyChars = [];

let nonEmptyNotifySeen = false;
let preferWriteWithResponse = false;

let lastDecodedMeasurementAtMs = 0;
let lastDecodedSetTimeText = "";

let lastDbPayload = null; // Uint8Array
let lastDbRecordsCount = null;

let requestedRecordsTransfer = false;

let lastRecord = {
  whenLocalText: "",
  whenKey: -1,
  prandial: "—",
  glucoseMgDl: null,
  ketoneMmolL: null,
};

// Raw records (glucose + ketone) received via record-transfer frames.
// We'll pair them into "readings" by matching glucose+ketone that are within
// a small time window (see READING_PAIR_WINDOW_MINUTES).
const READING_PAIR_WINDOW_MINUTES = 15;

let rawRecords = []; // Array<{tMin, whenKey, whenLocalText, prandial, kind, raw, value, type, record9Hex}>
let rawRecordKeys = new Set();
let readings = []; // Derived readings for UI/export.

function round1(x) {
  return Math.round(Number(x) * 10) / 10;
}

function epochMinutesFromDecoded(d) {
  // Device timestamps are local clock values; using UTC keeps deltas stable.
  return Math.floor(Date.UTC(d.year, (d.month || 1) - 1, d.day || 1, d.hour || 0, d.minute || 0) / 60000);
}

function recomputeReadingsFromRaw(windowMinutes = READING_PAIR_WINDOW_MINUTES) {
  const glus = [];
  const kets = [];
  for (const r of rawRecords) {
    if (r.kind === "GLUCOSE") glus.push(r);
    else if (r.kind === "KETONE") kets.push(r);
  }

  glus.sort((a, b) => a.tMin - b.tMin);
  kets.sort((a, b) => a.tMin - b.tMin);

  const usedKet = new Set();
  const out = [];

  const choosePrandial = (a, b) => {
    const pa = a?.prandial || "GENERAL";
    const pb = b?.prandial || "GENERAL";
    if (pa !== "GENERAL") return pa;
    if (pb !== "GENERAL") return pb;
    return "GENERAL";
  };

  for (const g of glus) {
    let best = null;
    let bestAbs = Infinity;
    for (let i = 0; i < kets.length; i++) {
      if (usedKet.has(i)) continue;
      const k = kets[i];
      const dt = Math.abs(k.tMin - g.tMin);
      if (dt > windowMinutes) continue;
      if (dt < bestAbs) {
        bestAbs = dt;
        best = { idx: i, rec: k };
      }
    }

    if (best) {
      usedKet.add(best.idx);
      const k = best.rec;
      const prandial = choosePrandial(g, k);
      const sortKey = Math.max(g.whenKey, k.whenKey);
      out.push({
        whenLocalText: g.whenLocalText || k.whenLocalText,
        prandial,
        sortKey,
        glucoseMgDl: Math.round(Number(g.value)),
        ketoneMmolL: round1(Number(k.value)),
        gluRecord9: g.record9Hex || "",
        ketRecord9: k.record9Hex || "",
      });
    } else {
      out.push({
        whenLocalText: g.whenLocalText,
        prandial: g.prandial || "GENERAL",
        sortKey: g.whenKey,
        glucoseMgDl: Math.round(Number(g.value)),
        ketoneMmolL: null,
        gluRecord9: g.record9Hex || "",
        ketRecord9: "",
      });
    }
  }

  for (let i = 0; i < kets.length; i++) {
    if (usedKet.has(i)) continue;
    const k = kets[i];
    out.push({
      whenLocalText: k.whenLocalText,
      prandial: k.prandial || "GENERAL",
      sortKey: k.whenKey,
      glucoseMgDl: null,
      ketoneMmolL: round1(Number(k.value)),
      gluRecord9: "",
      ketRecord9: k.record9Hex || "",
    });
  }

  out.sort((a, b) => b.sortKey - a.sortKey);
  readings = out;
}

function hex2(u8) {
  if (!(u8 instanceof Uint8Array)) return "";
  return Array.from(u8)
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

function getRequestedRecordsCount() {
  const nRaw = Number(el.recordsToFetch?.value);
  if (!Number.isFinite(nRaw)) return 1;
  return Math.max(1, Math.min(1000, Math.floor(nRaw)));
}

async function requestRecordsTransfer(n, label) {
  // The meter emits glucose and ketone as separate records.
  // If the user asks for "1", they usually mean "latest complete reading",
  // which requires 2 records (one glucose + one ketone) with the same timestamp.
  const nWire = n === 1 ? 2 : n;
  const cmd = buildLatestRecords16Command(nWire);
  await writeFrame(cmd, nWire === n ? label : `${label} (wire_n=${nWire})`);
  if (writeCharAlt && writeCharAlt !== writeChar) {
    const prev = writeChar;
    writeChar = writeCharAlt;
    try {
      await writeFrame(cmd, nWire === n ? `${label} (alt write)` : `${label} (wire_n=${nWire}, alt write)`);
    } finally {
      writeChar = prev;
    }
  }
}

function clearDisplayedRecords() {
  rawRecords = [];
  rawRecordKeys = new Set();
  readings = [];
  lastRecord = { whenLocalText: "", whenKey: -1, prandial: "—", glucoseMgDl: null, ketoneMmolL: null };
  updateValuesUiFromLastRecord();
}

function splitVivaFrames(u8) {
  // Some notifications contain multiple 0x7b..0x7d frames concatenated.
  const frames = [];
  for (let i = 0; i < u8.length; i++) {
    if (u8[i] !== 0x7b) continue;
    let j = i + 1;
    while (j < u8.length && u8[j] !== 0x7d) j++;
    if (j < u8.length && u8[j] === 0x7d) {
      frames.push(u8.slice(i, j + 1));
      i = j;
    }
  }
  return frames.length ? frames : [u8];
}

let logT0Ms = performance.now();
const LOG_COLS = {
  time: 12,
  message: 46,
  direction: 22,
};

function clampCell(s, width) {
  if (!s) return "".padEnd(width, " ");
  s = String(s);
  if (s.length === width) return s;
  if (s.length < width) return s.padEnd(width, " ");
  return s.slice(0, Math.max(0, width - 1)) + "…";
}

function formatRelTimeSeconds() {
  const s = (performance.now() - logT0Ms) / 1000;
  return s.toFixed(6);
}

function toHexSpaced(u8) {
  if (!(u8 instanceof Uint8Array)) return "";
  return Array.from(u8)
    .map((b) => b.toString(16).padStart(2, "0"))
    .join(" ");
}

function toHexCompact(u8) {
  if (!(u8 instanceof Uint8Array)) return "";
  return Array.from(u8)
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

function resetLog() {
  logT0Ms = performance.now();
  el.log.textContent = "";
  if (el.setTime) el.setTime.textContent = "—";
  lastDecodedSetTimeText = "";
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
  log(`App version ${APP_VERSION}`, "", "", 0);
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

function setStatus(text) {
  el.status.textContent = text;
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

function crc16Modbus(u8) {
  // CRC-16/MODBUS poly=0xA001, init=0xFFFF.
  let crc = 0xffff;
  for (let i = 0; i < u8.length; i++) {
    crc ^= u8[i] & 0xff;
    for (let j = 0; j < 8; j++) {
      if (crc & 1) crc = (crc >>> 1) ^ 0xa001;
      else crc = crc >>> 1;
    }
    crc &= 0xffff;
  }
  return crc & 0xffff;
}

function crcSendBytesFromCrc16(crc16) {
  // Match APK behavior:
  // - format as 4 hex digits
  // - map each digit to nibble byte 0..15
  // - swap pairs: [n0,n1,n2,n3] -> [n2,n3,n0,n1]
  const s = crc16.toString(16).padStart(4, "0").toUpperCase();
  const n0 = parseInt(s[0], 16);
  const n1 = parseInt(s[1], 16);
  const n2 = parseInt(s[2], 16);
  const n3 = parseInt(s[3], 16);
  return new Uint8Array([n2, n3, n0, n1]);
}

function buildLatestRecords16Command(param) {
  // Body hex: "0110012016550002" + "%04x"(param)
  // On-wire: 0x7B + body + crcSendBytes(crc16_modbus(body)) + 0x7D
  const p = Math.max(0, Math.min(1000, Number(param) | 0));
  const bodyHex = REQ_RECORDS_16_BODY_PREFIX_HEX + p.toString(16).padStart(4, "0");
  const body = fromHex(bodyHex);
  const crc = crc16Modbus(body);
  const crcBytes = crcSendBytesFromCrc16(crc);

  const out = new Uint8Array(1 + body.length + crcBytes.length + 1);
  out[0] = 0x7b;
  out.set(body, 1);
  out.set(crcBytes, 1 + body.length);
  out[out.length - 1] = 0x7d;
  return out;
}

async function copyDebugLogToClipboard() {
  const text = el.log?.textContent ?? "";
  if (!text) return;

  if (navigator.clipboard?.writeText && window.isSecureContext) {
    await navigator.clipboard.writeText(text);
    return;
  }

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

function buildSampleCopyText() {
  const payloadHex = toHexCompact(lastDbPayload);
  const recordsCount = lastDbRecordsCount;
  const setTime = el.setTime?.textContent?.trim() || "—";
  const deviceName = el.device?.textContent?.trim() || "—";

  const nowLocal = new Date();
  const nowText = `${nowLocal.getFullYear()}-${pad2(nowLocal.getMonth() + 1)}-${pad2(nowLocal.getDate())} ${pad2(nowLocal.getHours())}:${pad2(nowLocal.getMinutes())}`;

  const lines = [];
  lines.push("GK+ capture snippet + log:");
  lines.push("");
  lines.push(`${nowText} (local)`);
  lines.push(`device: ${deviceName}`);
  lines.push(`set time (decoded): ${setTime}`);
  if (payloadHex) {
    lines.push(`0xdb payload (hex): ${payloadHex}`);
  } else {
    lines.push("0xdb payload (hex): ");
  }
  lines.push(`0xdb decoded: records_count=${recordsCount ?? ""}`);
  lines.push("");
  lines.push("log");
  lines.push("```text");
  lines.push((el.log?.textContent ?? "").trimEnd());
  lines.push("```");
  lines.push("");
  lines.push("Copilot reminder:");
  lines.push("- Store this as a new sample row in GK+_SAMPLES.markdown");
  lines.push("- Re-run tools/gkplus_decode_test.py --hex <payload> to confirm records_count + CRC");
  lines.push("- Capture record-transfer frames (records-mode) so we can decode actual measurements + timestamps");

  return lines.join("\n");
}

async function copySampleAndLogToClipboard() {
  const text = buildSampleCopyText();
  if (!text) return;

  if (navigator.clipboard?.writeText && window.isSecureContext) {
    await navigator.clipboard.writeText(text);
    return;
  }

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

function computeShownGki(glucoseMgDl, ketoneMmolL) {
  if (glucoseMgDl === null || ketoneMmolL === null) return null;
  const k = Number(ketoneMmolL);
  if (!Number.isFinite(k) || k <= 0) return null;
  const g = Number(glucoseMgDl) / 18.0;
  const gki = g / k;
  return Math.floor(gki * 10) / 10;
}

function buildReadingsTableMarkdown(maxRows = 5) {
  const rows = [];

  // Ensure derived readings are up to date.
  recomputeReadingsFromRaw();

  for (const rec of readings) {
    if (rec.glucoseMgDl === null || rec.ketoneMmolL === null) continue;
    rows.push(rec);
  }

  const take = rows.slice(0, Math.max(1, Math.min(50, Number(maxRows) || 5)));
  const out = [];
  out.push("| Reading time (device) | Tag | Decoded glucose (mg/dL) | Decoded ketone (mmol/L) | Decoded GKI | Official glucose | Official ketone | Official GKI | Official time | Notes |");
  out.push("|---|---|---:|---:|---:|---:|---:|---:|---|---|");

  for (const r of take) {
    const glu = r.glucoseMgDl;
    const ket = r.ketoneMmolL;
    const gki = computeShownGki(glu, ket);

    const notesParts = [];
    if (r.gluRecord9) notesParts.push(`glu9=${r.gluRecord9}`);
    if (r.ketRecord9) notesParts.push(`ket9=${r.ketRecord9}`);
    const notes = notesParts.join(" ");

    out.push(
      `| ${r.whenLocalText || ""} | ${r.prandial || ""} | ${glu ?? ""} | ${ket !== null ? Number(ket).toFixed(1) : ""} | ${gki !== null ? Number(gki).toFixed(1) : ""} |  |  |  |  | ${notes} |`
    );
  }

  return out.join("\n");
}

async function copyReadingsTableToClipboard() {
  const text = buildReadingsTableMarkdown(5);
  if (!text) return;

  if (navigator.clipboard?.writeText && window.isSecureContext) {
    await navigator.clipboard.writeText(text);
    return;
  }

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

function pad2(n) {
  return String(n).padStart(2, "0");
}

function formatYmdHms(y, mo, d, h, mi, s) {
  return `${y}-${pad2(mo)}-${pad2(d)} ${pad2(h)}:${pad2(mi)}:${pad2(s)}`;
}

function formatYmdHm(y, mo, d, h, mi) {
  return `${y}-${pad2(mo)}-${pad2(d)} ${pad2(h)}:${pad2(mi)}`;
}

function decodeVivachekRecordSnippet9(record9) {
  // APK-derived record snippet layout (9 bytes):
  //   [0:5] YY MM DD HH mm (YY is years since 2000)
  //   [5:7] value base-100 => raw=b5*100+b6
  //   [7]   flags: high nibble is prandial (1=PRE,2=POST,else GENERAL)
  //   [8]   type: 0x11 glucose, 0x55 ketone, 0x56 ketone_gki, 0x12 glucose/gki
  if (!(record9 instanceof Uint8Array) || record9.length !== 9) return null;

  const yy = record9[0] & 0xff;
  const mo = record9[1] & 0xff;
  const dd = record9[2] & 0xff;
  const hh = record9[3] & 0xff;
  const mi = record9[4] & 0xff;
  const year = 2000 + yy;
  const whenKey = year * 100000000 + mo * 1000000 + dd * 10000 + hh * 100 + mi;

  const raw = (record9[5] & 0xff) * 100 + (record9[6] & 0xff);
  const flags = record9[7] & 0xff;
  const prNib = (flags >> 4) & 0x0f;
  const prandial = prNib === 1 ? "PRE" : prNib === 2 ? "POST" : "GENERAL";
  const type = record9[8] & 0xff;

  let kind = "UNKNOWN";
  let value = null;
  let unit = "";
  if (type === 0x11 || type === 0x12 || type === 0x22) {
    kind = "GLUCOSE";
    value = raw;
    unit = "mg/dL";
  } else if (type === 0x55 || type === 0x56 || type === 0x66) {
    kind = "KETONE";
    // APK: BloodValuesParser.parseBloodKetone(D) returns value/100.0
    value = raw / 100.0;
    unit = "mmol/L";
  }

  return {
    year,
    month: mo,
    day: dd,
    hour: hh,
    minute: mi,
    whenLocalText: formatYmdHm(year, mo, dd, hh, mi),
    whenKey,
    raw,
    flags,
    prandial,
    type,
    kind,
    value,
    unit,
  };
}

function decodeRecordTransferFrame(payload) {
  // Matches notify frames like:
  //   7b 01 20 01 10 16 aa 00 09 <record9> <crc4> 7d
  if (!(payload instanceof Uint8Array) || payload.length < 23) return null;
  if (payload[0] !== 0x7b || payload[payload.length - 1] !== 0x7d) return null;

  const cmd = payload[5] & 0xff;
  if (cmd !== 0x16 && cmd !== 0xdd && cmd !== 0xde) return null;
  if ((payload[6] & 0xff) !== 0xaa) return null;
  if ((payload[7] & 0xff) !== 0x00) return null;
  if ((payload[8] & 0xff) !== 0x09) return null;

  // Extract the 9-byte record snippet at bytes [9:18].
  const record9 = payload.slice(9, 18);
  const decoded = decodeVivachekRecordSnippet9(record9);
  if (!decoded) return null;

  return { cmd, record9, decoded };
}

function updateValuesUiFromLastRecord() {
  // Show prandial + record time.
  const icon = lastRecord.prandial === "PRE" ? "🍎" : lastRecord.prandial === "POST" ? "🍏" : "—";
  const text = lastRecord.whenLocalText ? `${lastRecord.prandial} @ ${lastRecord.whenLocalText}` : lastRecord.prandial || "—";
  if (el.mealIcon) el.mealIcon.textContent = icon;
  if (el.mealText) el.mealText.textContent = text;

  if (el.glucose) {
    el.glucose.textContent = lastRecord.glucoseMgDl !== null ? `${lastRecord.glucoseMgDl} mg/dL` : "—";
  }
  if (el.ketone) {
    el.ketone.textContent =
      lastRecord.ketoneMmolL !== null ? `${Number(lastRecord.ketoneMmolL).toFixed(1)} mmol/L` : "—";
  }
  if (el.gki) {
    if (lastRecord.glucoseMgDl !== null && lastRecord.ketoneMmolL !== null && Number(lastRecord.ketoneMmolL) > 0) {
      const gki = computeShownGki(lastRecord.glucoseMgDl, lastRecord.ketoneMmolL);
      el.gki.textContent = gki !== null ? Number(gki).toFixed(1) : "—";
    } else {
      el.gki.textContent = "—";
    }
  }
}

function recomputeLastRecordSelection() {
  recomputeReadingsFromRaw();

  // Prefer the most recent derived reading that has BOTH glucose and ketone.
  let bestComplete = null;
  let bestAny = null;
  for (const rec of readings) {
    const hasAny = rec.glucoseMgDl !== null || rec.ketoneMmolL !== null;
    const hasBoth = rec.glucoseMgDl !== null && rec.ketoneMmolL !== null;
    if (hasBoth && (!bestComplete || rec.sortKey > bestComplete.sortKey)) bestComplete = rec;
    if (hasAny && (!bestAny || rec.sortKey > bestAny.sortKey)) bestAny = rec;
  }
  const chosen = bestComplete || bestAny;
  if (!chosen) return;
  lastRecord = {
    whenLocalText: chosen.whenLocalText || "",
    whenKey: chosen.sortKey,
    prandial: chosen.prandial || "—",
    glucoseMgDl: chosen.glucoseMgDl ?? null,
    ketoneMmolL: chosen.ketoneMmolL ?? null,
  };
  updateValuesUiFromLastRecord();
}

function decodeSetTime44(payload) {
  // Observed central write (pcaps):
  //   7b 01 10 01 20 44 66 00 06 YY MM DD HH MM SS ?? ?? ?? ?? 7d
  // Where YY is years since 2000.
  if (!(payload instanceof Uint8Array) || payload.length < 16) return null;
  if (payload[0] !== 0x7b || payload[payload.length - 1] !== 0x7d) return null;
  if (payload[5] !== 0x44) return null;

  // This specific form (0x66 + length 0x0006) is the one we know how to decode.
  if (payload[6] !== 0x66 || payload[7] !== 0x00 || payload[8] !== 0x06) return null;

  const yy = payload[9] & 0xff;
  const mo = payload[10] & 0xff;
  const dd = payload[11] & 0xff;
  const hh = payload[12] & 0xff;
  const mi = payload[13] & 0xff;
  const ss = payload[14] & 0xff;

  const year = 2000 + yy;
  if (mo < 1 || mo > 12) return null;
  if (dd < 1 || dd > 31) return null;
  if (hh > 23) return null;
  if (mi > 59) return null;
  if (ss > 59) return null;

  return {
    year,
    month: mo,
    day: dd,
    hour: hh,
    minute: mi,
    second: ss,
    text: formatYmdHms(year, mo, dd, hh, mi, ss),
  };
}

function buildSetTime44Frame(dateObj) {
  // Builds:
  //   7b 01 10 01 20 44 66 00 06 YY MM DD HH MM SS <suffix4> 7d
  // where YY is years since 2000 and time fields are *local time*.
  const d = dateObj instanceof Date ? dateObj : new Date();
  const yy = (d.getFullYear() - 2000) & 0xff;
  const mo = (d.getMonth() + 1) & 0xff;
  const dd = d.getDate() & 0xff;
  const hh = d.getHours() & 0xff;
  const mi = d.getMinutes() & 0xff;
  const ss = d.getSeconds() & 0xff;

  const out = new Uint8Array(20);
  out.set([0x7b, 0x01, 0x10, 0x01, 0x20, 0x44, 0x66, 0x00, 0x06], 0);
  out.set([yy, mo, dd, hh, mi, ss], 9);
  out.set(REQ_SET_TIME_44_SUFFIX, 15);
  out[19] = 0x7d;
  return out;
}

function decodeDbRecordsCount(payload) {
  // Official APK (Keto-Mojo Classic 2.6.6) treats 0xdb as records-count.
  // Response shape (notify handle 0x001f):
  //   7b 01 20 01 10 db aa 00 02 <count2> <crc4> 7d
  // Count is base-100 in two bytes: count = b0*100 + b1.
  if (!(payload instanceof Uint8Array) || payload.length < 16) return null;
  if (payload[0] !== 0x7b || payload[payload.length - 1] !== 0x7d) return null;
  if (payload[5] !== 0xdb) return null;
  if (payload[6] !== 0xaa) return null;
  if (payload.length < 11) return null;

  const b0 = payload[9] & 0xff;
  const b1 = payload[10] & 0xff;
  return { count: b0 * 100 + b1, b0, b1 };
}

function processRxPayload(u8, sourceLabel) {
  if (!(u8 instanceof Uint8Array)) {
    return;
  }
  if (u8.byteLength === 0) {
    return;
  }

  const frames = splitVivaFrames(u8);
  if (frames.length > 1) {
    for (let i = 0; i < frames.length; i++) {
      processRxPayload(frames[i], `${sourceLabel}#${i + 1}`);
    }
    return;
  }

  const decodedSetTime = decodeSetTime44(u8);
  if (decodedSetTime) {
    if (decodedSetTime.text !== lastDecodedSetTimeText) {
      lastDecodedSetTimeText = decodedSetTime.text;
      if (el.setTime) el.setTime.textContent = decodedSetTime.text;
    }
    log(`${sourceLabel}: setTime=${decodedSetTime.text}`, "", "");
    return;
  }

  const decodedCount = decodeDbRecordsCount(u8);
  if (decodedCount) {
    lastDecodedMeasurementAtMs = performance.now();
    lastDbPayload = u8;
    lastDbRecordsCount = decodedCount.count;
    log(`${sourceLabel}: records_count=${decodedCount.count} (b0=${decodedCount.b0} b1=${decodedCount.b1})`, "", "");

    // Records count is metadata; do not clear measurement UI here.
    el.lastRx.textContent = toHexSpaced(u8);
    if (el.recordId) el.recordId.textContent = String(decodedCount.count);

    // Best-effort: trigger record-transfer download once per connection.
    // The official SDK issues a parameterized 0x16 read-records command.
    if (!requestedRecordsTransfer && writeChar) {
      requestedRecordsTransfer = true;
      const n = getRequestedRecordsCount();
      // Fire-and-forget: this is inside a sync notify handler.
      requestRecordsTransfer(n, `Req 0x16 (read records, n=${n})`).catch((e) => {
        log(`Req 0x16 failed: ${String(e)}`, "", "");
      });
    }
    return;
  }

  const recordXfer = decodeRecordTransferFrame(u8);
  if (recordXfer) {
    const r = recordXfer.decoded;
    // Keep showing last RX.
    el.lastRx.textContent = toHexSpaced(u8);

    // Record-transfer notifications can deliver glucose and ketone at slightly
    // different timestamps; treat them as one reading if within the pairing window.
    if (r.value !== null) {
      const rec = {
        tMin: epochMinutesFromDecoded(r),
        whenKey: r.whenKey,
        whenLocalText: r.whenLocalText,
        prandial: r.prandial,
        kind: r.kind,
        raw: r.raw,
        value: Number(r.value),
        type: r.type,
        record9Hex: hex2(recordXfer.record9),
      };
      const dedupeKey = `${rec.kind}|${rec.whenKey}|${rec.raw}|${rec.type}`;
      if (!rawRecordKeys.has(dedupeKey)) {
        rawRecordKeys.add(dedupeKey);
        rawRecords.push(rec);
      }
      recomputeLastRecordSelection();
    }

    const kindShort = r.kind === "GLUCOSE" ? "GLU" : r.kind === "KETONE" ? "KET" : "REC";
    const vTxt = r.value === null ? "" : r.kind === "GLUCOSE" ? `${Math.round(r.value)} mg/dL` : `${Number(r.value).toFixed(3)} mmol/L`;
    log(`${sourceLabel}: record ${kindShort} ${vTxt} ${r.prandial} ${r.whenLocalText} type=0x${r.type.toString(16).padStart(2, "0")}`, "", "");
    return;
  }

  // For other messages, still show last RX.
  el.lastRx.textContent = toHexSpaced(u8);
}

async function readAndProcessCharacteristicValue(ch, label) {
  if (!ch) {
    return;
  }
  try {
    const dv = await ch.readValue();
    const u8 = new Uint8Array(dv.buffer.slice(dv.byteOffset, dv.byteOffset + dv.byteLength));
    log(label, "", u8);
    processRxPayload(u8, label);
  } catch (e) {
    log(`${label} failed: ${String(e)}`, "", "");
  }
}

async function onNotify(evt) {
  const DIR_RX = "🟩P→C";
  const ch = evt?.target;
  const dv = ch?.value;
  const chLabel = ch?.uuid ? `Notify(${ch.uuid})` : "Notify";

  if (!dv) {
    log(chLabel, DIR_RX, "(no value)");
    return;
  }

  const u8 = new Uint8Array(dv.buffer.slice(dv.byteOffset, dv.byteOffset + dv.byteLength));
  log(chLabel, DIR_RX, u8);

  if (u8.byteLength === 0) {
    log(
      `${chLabel}: empty payload (dv.byteLength=${dv.byteLength} dv.buffer.byteLength=${dv.buffer?.byteLength ?? "?"})`,
      "",
      ""
    );

    // Some devices signal via notify but require a read of another attribute.
    // Only attempt a read if the characteristic is readable.
    if (ch?.properties?.read) {
      await readAndProcessCharacteristicValue(ch, `Read after empty notify (${ch?.uuid ?? "?"})`);
    } else {
      log(`${chLabel}: not readable; skipping readValue() fallback`, "", "");
    }
    return;
  }

  nonEmptyNotifySeen = true;
  processRxPayload(u8, chLabel);
}

async function writeFrame(u8, label) {
  if (!writeChar) {
    throw new Error("Write characteristic not ready");
  }
  const DIR_TX = "🟦C→P";
  log(label, DIR_TX, u8);

  const decodedSetTime = decodeSetTime44(u8);
  if (decodedSetTime) {
    if (decodedSetTime.text !== lastDecodedSetTimeText) {
      lastDecodedSetTimeText = decodedSetTime.text;
      if (el.setTime) el.setTime.textContent = decodedSetTime.text;
    }
    log("Set time (decoded)", DIR_TX, decodedSetTime.text);
  }

  const canNoRsp = !!writeChar.properties?.writeWithoutResponse;
  const canRsp = !!writeChar.properties?.write;

  if (preferWriteWithResponse && canRsp) {
    await writeChar.writeValueWithResponse(u8);
    return;
  }
  if (canNoRsp) {
    await writeChar.writeValueWithoutResponse(u8);
    return;
  }
  if (canRsp) {
    await writeChar.writeValueWithResponse(u8);
    return;
  }
  throw new Error("Write characteristic does not support write/writeWithoutResponse");
}

async function dumpServicesAndChars(server) {
  try {
    const services = await server.getPrimaryServices();
    log("Primary services", "", services.map((s) => s.uuid).join(" | "));
    for (const s of services) {
      try {
        const chars = await s.getCharacteristics();
        const describeChar = (c) => {
          const p = c.properties;
          const flags = [
            p?.notify ? "notify" : null,
            p?.indicate ? "indicate" : null,
            p?.writeWithoutResponse ? "writeNoRsp" : null,
            p?.write ? "write" : null,
            p?.read ? "read" : null,
          ]
            .filter(Boolean)
            .join(",");
          return `${c.uuid} [${flags}]`;
        };
        log(`Service ${s.uuid} characteristics`, "", chars.map(describeChar).join(" | "));
      } catch (e) {
        log(`Service ${s.uuid} getCharacteristics failed: ${String(e)}`, "", "");
      }
    }
  } catch (e) {
    log(`getPrimaryServices failed: ${String(e)}`, "", "");
  }
}

async function subscribeAllNotifyChars(server) {
  const out = [];
  let services = [];
  try {
    services = await server.getPrimaryServices();
  } catch {
    services = [];
  }

  for (const s of services) {
    let chars = [];
    try {
      chars = await s.getCharacteristics();
    } catch {
      continue;
    }
    for (const c of chars) {
      const p = c.properties;
      if (!(p?.notify || p?.indicate)) continue;
      try {
        await c.startNotifications();
        c.addEventListener("characteristicvaluechanged", onNotify);
        out.push(c);
        log("Subscribed", "", `${s.uuid} :: ${c.uuid}`);
      } catch (e) {
        log("Subscribe failed", "", `${s.uuid} :: ${c.uuid} :: ${String(e)}`);
      }
    }
  }
  return out;
}

async function connect() {
  resetLog();
  setStatus("Requesting device…");
  requestedRecordsTransfer = false;
  rawRecords = [];
  rawRecordKeys = new Set();
  readings = [];
  lastRecord = { whenLocalText: "", whenKey: -1, prandial: "—", glucoseMgDl: null, ketoneMmolL: null };
  updateValuesUiFromLastRecord();

  // Web Bluetooth chooser filters can be flaky depending on what the OS/browser
  // considers "advertised". Strategy:
  // 1) Try a tight namePrefix filter (fast, short device list)
  // 2) If no matches, fall back to acceptAllDevices
  // 3) Always validate *after* connecting by checking for the vendor service
  const optionalServices = [GK_SERVICE_UUID_16, GK_SERVICE_UUID_PCAP, GK_SERVICE_UUID];

  try {
    device = await navigator.bluetooth.requestDevice({
      filters: [{ namePrefix: GK_DEVICE_NAME_PREFIX }],
      optionalServices,
    });
  } catch (e) {
    const name = e?.name || "";
    if (name === "NotFoundError") {
      log("namePrefix filter found no devices; falling back to acceptAllDevices", "", "");
      log("Tip: pick the GK+ (often shown as 'Keto-Mojo GK+ Meter - Paired')", "", "");
      device = await navigator.bluetooth.requestDevice({
        acceptAllDevices: true,
        optionalServices,
      });
    } else {
      throw e;
    }
  }

  el.device.textContent = `${device.name ?? "(unnamed)"} (${device.id})`;
  log(`Picked device: ${device.name ?? "(unnamed)"}`, "", device.id);
  device.addEventListener("gattserverdisconnected", onDisconnected);

  setStatus("Connecting…");
  gattServer = await device.gatt.connect();

  setStatus("Discovering service…");

  await dumpServicesAndChars(gattServer);

  // Optional fallback path: some devices expose a wrapper 0xFFF0 service.
  // We keep its writable characteristic around as an alternative transport.
  try {
    const svc = await gattServer.getPrimaryService(GK_SERVICE_UUID_16);
    const chars = await svc.getCharacteristics();
    writeCharAlt =
      chars.find((c) => c.uuid === "0000fff3-0000-1000-8000-00805f9b34fb") ||
      chars.find((c) => c.properties?.write || c.properties?.writeWithoutResponse) ||
      null;
    if (writeCharAlt) {
      log(`Alt write char ${writeCharAlt.uuid}`, "", "");
    }
  } catch {
    writeCharAlt = null;
  }

  // Prefer the pcap-derived 128-bit vendor service if available. The device may
  // also expose a 16-bit wrapper service (FFF0) that isn't the real data path.
  let service = null;
  try {
    service = await gattServer.getPrimaryService(GK_SERVICE_UUID_PCAP);
    activeServiceUuid = GK_SERVICE_UUID_PCAP;
  } catch {
    try {
      service = await gattServer.getPrimaryService(GK_SERVICE_UUID_16);
      activeServiceUuid = GK_SERVICE_UUID_16;
    } catch {
      try {
        service = await gattServer.getPrimaryService(GK_SERVICE_UUID);
        activeServiceUuid = GK_SERVICE_UUID;
      } catch {
        log("Selected device does not expose an expected GK+ vendor service", "", "");
        try {
          device.gatt?.disconnect();
        } catch {
          // Ignore.
        }
        throw new Error(
          "Selected device is not a GK+ (vendor service not found). Try Connect again and pick a different device."
        );
      }
    }
  }

  // Dynamic characteristic selection by properties. This also lets us log the
  // actual UUIDs in case we want to tighten filters later.
  const chars = await service.getCharacteristics();
  {
    const describeChar = (c) => {
      const p = c.properties;
      const flags = [
        p?.notify ? "notify" : null,
        p?.indicate ? "indicate" : null,
        p?.writeWithoutResponse ? "writeNoRsp" : null,
        p?.write ? "write" : null,
        p?.read ? "read" : null,
      ]
        .filter(Boolean)
        .join(",");
      return `${c.uuid} [${flags}]`;
    };
    log("Discovered characteristics", "", chars.map(describeChar).join(" | "));
  }

  const notifyCandidates = chars.filter((c) => c.properties?.notify || c.properties?.indicate);
  const writeCandidates = chars.filter((c) => c.properties?.writeWithoutResponse || c.properties?.write);

  // Prefer UUIDs seen in pcaps (128-bit), then older hypothesis, then fall back.
  notifyChar =
    chars.find((c) => c.uuid === GK_NOTIFY_CHAR_UUID_PCAP) ||
    chars.find((c) => c.uuid === GK_NOTIFY_CHAR_UUID) ||
    notifyCandidates[0] ||
    null;
  writeChar =
    chars.find((c) => c.uuid === GK_WRITE_CHAR_UUID_PCAP) ||
    chars.find((c) => c.uuid === GK_WRITE_CHAR_UUID) ||
    writeCandidates.find((c) => notifyChar && c.uuid !== notifyChar.uuid) ||
    writeCandidates[0] ||
    null;

  // If we only found one characteristic that supports both notify and write,
  // keep it, but log explicitly.
  if (notifyChar && writeChar && notifyChar.uuid === writeChar.uuid) {
    log("Note: notify/write are same characteristic", "", notifyChar.uuid);
  }

  if (!notifyChar || !writeChar) {
    const propsSummary = chars
      .map((c) => {
        const p = c.properties;
        const flags = [
          p?.notify ? "notify" : null,
          p?.indicate ? "indicate" : null,
          p?.writeWithoutResponse ? "writeNoRsp" : null,
          p?.write ? "write" : null,
          p?.read ? "read" : null,
        ]
          .filter(Boolean)
          .join(",");
        return `${c.uuid} [${flags}]`;
      })
      .join(" | ");
    log("Characteristic discovery failed", "", propsSummary);
    throw new Error("Could not find notify/write characteristics (see log for UUIDs)");
  }

  el.serviceUuid.textContent = activeServiceUuid;
  el.chars.textContent = `notify=${notifyChar.uuid} write=${writeChar.uuid}`;
  log(`Using service ${activeServiceUuid}`, "", "");
  log(`Using notify char ${notifyChar.uuid}`, "", "");
  log(`Using write  char ${writeChar.uuid}`, "", "");

  setStatus("Starting notifications…");

  // Subscribe broadly so we don't miss the real data characteristic.
  subscribedNotifyChars = await subscribeAllNotifyChars(gattServer);
  if (subscribedNotifyChars.length === 0) {
    // Fallback: try at least the chosen notifyChar.
    await notifyChar.startNotifications();
    notifyChar.addEventListener("characteristicvaluechanged", onNotify);
    subscribedNotifyChars = [notifyChar];
  }

  setStatus("Connected");

  el.connectBtn.disabled = true;
  el.disconnectBtn.disabled = false;
  if (el.fetchBtn) el.fetchBtn.disabled = false;

  // Behave like the user clicked "Fetch" immediately upon connect.
  // Default is 1 (which requests 2 records on-wire to get a complete reading).
  {
    const n = getRequestedRecordsCount();
    clearDisplayedRecords();
    requestRecordsTransfer(n, `Req 0x16 (auto fetch on connect, n=${n})`).catch((e) => {
      log(`Auto fetch failed: ${String(e)}`, "", "");
    });
  }

  // Replicate the observed request sequence (best-effort).
  try {
    // Official app sets the meter time (0x44) during the session.
    // Send a best-effort set-time using the on-wire shape observed in pcaps.
    await writeFrame(buildSetTime44Frame(new Date()), "Req 0x44 (set time)");
    await sleep(80);

    await writeFrame(REQ_INFO_66, "Req 0x66 (info)");
    await sleep(80);
    await writeFrame(REQ_INIT_AA, "Req 0xaa (init)");
    await sleep(80);
    await writeFrame(REQ_INFO_77, "Req 0x77 (info)");
    await sleep(120);
    await writeFrame(REQ_INFO_66, "Req 0x66 (info)");
    await sleep(120);
  } catch (e) {
    log(`Info/init writes failed: ${String(e)}`, "", "");
  }

  // Poll for records count a few times (pcaps show repeated 0xdb requests).
  for (let i = 0; i < 6; i++) {
    await writeFrame(REQ_RECORDS_COUNT_DB, `Req 0xdb (records count) #${i + 1}`);
    await sleep(220);

    // If the peripheral doesn't push a full payload via notifications, try a
    // read shortly after the request.
    if (performance.now() - lastDecodedMeasurementAtMs > 200) {
      await readAndProcessCharacteristicValue(notifyChar, "Read measurement (fallback)");
    }

    await sleep(320);
  }

  // If we never saw a non-empty notification, try switching write mode and
  // polling once more (some firmwares may require write-with-response).
  if (!nonEmptyNotifySeen && writeChar?.properties?.write) {
    preferWriteWithResponse = true;
    log("No non-empty notifications seen; retrying with write-with-response", "", "");
    for (let i = 0; i < 2; i++) {
      await writeFrame(REQ_RECORDS_COUNT_DB, `Req 0xdb (records count, with response) #${i + 1}`);
      await sleep(350);
    }
  }
}

async function disconnect() {
  if (!device) {
    return;
  }
  setStatus("Disconnecting…");

  try {
    for (const c of subscribedNotifyChars) {
      try {
        c.removeEventListener("characteristicvaluechanged", onNotify);
        await c.stopNotifications();
      } catch {
        // Ignore.
      }
    }
  } catch {
    // Ignore.
  }

  try {
    if (device.gatt?.connected) {
      device.gatt.disconnect();
    }
  } catch {
    // Ignore.
  }

  onDisconnected();
}

function onDisconnected() {
  setStatus("Disconnected");
  el.connectBtn.disabled = false;
  el.disconnectBtn.disabled = true;

  gattServer = null;
  notifyChar = null;
  writeChar = null;
  writeCharAlt = null;
  device = null;

  subscribedNotifyChars = [];
  nonEmptyNotifySeen = false;
  preferWriteWithResponse = false;
  requestedRecordsTransfer = false;

  if (el.fetchBtn) el.fetchBtn.disabled = true;

  lastRecord = {
    whenLocalText: "",
    whenKey: -1,
    prandial: "—",
    glucoseMgDl: null,
    ketoneMmolL: null,
  };
  rawRecords = [];
  rawRecordKeys = new Set();
  readings = [];
  updateValuesUiFromLastRecord();
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

el.connectBtn.addEventListener("click", () => {
  connect().catch((err) => {
    log(`Connect failed: ${String(err)}`, "", "");
    setStatus(`Error: ${String(err)}`);
  });
});

el.disconnectBtn.addEventListener("click", () => {
  disconnect().catch((err) => {
    log(`Disconnect failed: ${String(err)}`, "", "");
    setStatus(`Error: ${String(err)}`);
  });
});

el.copyLogBtn.addEventListener("click", () => {
  copyDebugLogToClipboard().catch((err) => {
    log(`Copy failed: ${String(err)}`, "", "");
  });
});

el.copySampleBtn?.addEventListener("click", () => {
  copySampleAndLogToClipboard().catch((err) => {
    log(`Copy sample failed: ${String(err)}`, "", "");
  });
});

el.copyReadingsBtn?.addEventListener("click", () => {
  copyReadingsTableToClipboard().catch((err) => {
    log(`Copy readings failed: ${String(err)}`, "", "");
  });
});

el.fetchBtn?.addEventListener("click", () => {
  const n = getRequestedRecordsCount();
  // Clear prior accumulation so the UI reflects the new fetch.
  clearDisplayedRecords();
  requestRecordsTransfer(n, `Req 0x16 (manual fetch, n=${n})`).catch((e) => {
    log(`Manual fetch failed: ${String(e)}`, "", "");
  });
});

// Initial UI
resetLog();
el.serviceUuid.textContent = GK_SERVICE_UUID_16;
el.chars.textContent = `notify=${GK_NOTIFY_CHAR_UUID} write=${GK_WRITE_CHAR_UUID}`;
