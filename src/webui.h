// Embedded browser dashboard served at "/" by ApiServer: live SPL readouts,
// metric grid, and RTA bars over the /api/stream WebSocket. Self-contained
// (no external resources) so it works on closed show networks.
#pragma once

inline const char *kDashboardHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ProdMesh Remote RTA</title>
<style>
  :root {
    --bg: #14161c; --panel: #20242e; --border: #2a2f3d; --grid: #2a2e39;
    --text: #c8cede; --dim: #8a92a6; --value: #e8ecf4;
    --bar: #2fbf9b; --bartop: #5ce0bd; --warn: #e8c84b; --alert: #e05c5c;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg); color: var(--text);
    font-family: -apple-system, "Segoe UI", Roboto, sans-serif;
    padding: 14px; max-width: 1100px; margin: 0 auto;
  }
  header { display: flex; align-items: baseline; gap: 12px; margin-bottom: 14px; }
  header h1 { font-size: 18px; color: var(--value); font-weight: 600; }
  #status { font-size: 12px; color: var(--dim); }
  #status.live::before { content: "● "; color: var(--bar); }
  #status.down::before { content: "● "; color: var(--alert); }
  #modeinfo { font-size: 12px; color: var(--dim); margin-left: auto; }
  #alarmbanner {
    display: none; margin-bottom: 12px; padding: 8px 14px; border-radius: 6px;
    font-weight: 700; font-size: 15px; letter-spacing: 0.5px;
  }
  #alarmbanner.warn { display: block; background: #4a4020; color: var(--warn); }
  #alarmbanner.alert { display: block; background: #4a2020; color: var(--alert); }
  /* Signal loss outranks a level alarm: no audio is the worse problem.
     Solid, never flashing — the running timer carries the urgency. */
  #signalbanner {
    display: none; margin-bottom: 12px; padding: 12px 14px; border-radius: 6px;
    background: #4a2020; color: var(--alert); font-weight: 700; font-size: 19px;
    letter-spacing: 1px; text-align: center;
  }
  #signalbanner.on { display: block; }
  .bigrow { display: grid; grid-template-columns: repeat(auto-fit, minmax(170px, 1fr));
            gap: 10px; margin-bottom: 10px; }
  .tile {
    background: var(--panel); border: 1px solid var(--border);
    border-radius: 8px; padding: 10px 14px 12px;
  }
  .tile .cap { font-size: 11px; color: var(--dim); letter-spacing: 0.5px; }
  .tile .val {
    font-family: ui-monospace, Consolas, Menlo, monospace;
    font-size: 40px; font-weight: 700; color: var(--value); line-height: 1.15;
  }
  .tile .val.small { font-size: 24px; }
  .tile .val.warn { color: var(--warn); }
  .tile .val.alert { color: var(--alert); }
  .gridrow { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr));
             gap: 10px; margin-bottom: 14px; }
  .gauge { position: relative; height: 8px; margin-top: 6px; display: none; }
  .gauge .track { position: absolute; left: 0; right: 0; top: 3px; height: 2px;
                  background: var(--grid); border-radius: 1px; }
  .gauge .band { position: absolute; top: 2px; height: 4px;
                 background: #3a6b5f; border-radius: 2px; }
  .gauge .dot { position: absolute; top: 0; width: 8px; height: 8px;
                border-radius: 50%; background: var(--bar); margin-left: -4px; }
  .gauge .dot.out { background: var(--warn); }
  #rtabox { background: var(--panel); border: 1px solid var(--border);
            border-radius: 8px; padding: 10px; }
  #rtabox .cap { font-size: 11px; color: var(--dim); margin-bottom: 6px; }
  canvas { width: 100%; height: 280px; display: block; }
  footer { margin-top: 10px; font-size: 11px; color: var(--dim); }
</style>
</head>
<body>
<header>
  <h1>ProdMesh Remote RTA</h1>
  <span id="status" class="down">connecting…</span>
  <span id="modeinfo"></span>
</header>
<div id="signalbanner"></div>
<div id="alarmbanner"></div>
<div class="bigrow" id="bigrow"></div>
<div class="gridrow" id="gridrow"></div>
<div id="rtabox">
  <div class="cap" id="rtacap">RTA — 1/3 OCTAVE</div>
  <canvas id="rta"></canvas>
</div>
<footer>Served by ProdMesh Remote RTA — levels update live over WebSocket.</footer>
<script>
"use strict";
// Tile sets per measurement mode. The app reports which one it is in, so the
// dashboard follows without needing to be told separately.
const TILES = {
  acoustic: {
    big: ["laf", "las", "leq"],
    small: ["leqS", "leqL", "lzpk", "lcpk", "ca", "l10", "l50", "l90",
            "doseN", "doseO"],
  },
  program: {
    big: ["lufsM", "lufsS", "lufsI"],
    small: ["toTarget", "lra", "plr", "monoDelta", "corr", "balance",
            "dbtp", "dbtpMax", "dbtpL", "dbtpR"],
  },
};
let MODE = "acoustic";
let IDS = [];
function caption(id, w, mode) {
  const dbfs = mode === "program";
  const map = {
    laf: "L" + w + "F (FAST)" + (dbfs ? " dBFS" : ""),
    las: "L" + w + "S (SLOW)" + (dbfs ? " dBFS" : ""),
    leq: "L" + w + "eq" + (dbfs ? " dBFS" : ""),
    leqS: "LAeq SHORT", leqL: "LAeq LONG",
    lzpk: "LZpk" + (dbfs ? " dBFS" : ""), lcpk: "LCpk",
    ca: "C-A RATIO", l10: "L10", l50: "L50", l90: "L90",
    doseN: "DOSE NIOSH", doseO: "DOSE OSHA",
    lufsM: "M — 400 ms (LUFS)", lufsS: "S — 3 s (LUFS)",
    lufsI: "INTEGRATED (LUFS)", toTarget: "Δ TARGET (LU)",
    dbtp: "TRUE PEAK (dBTP)", dbtpMax: "TP MAX (dBTP)", plr: "PLR (LU)",
    lra: "LRA (LU)", monoDelta: "MONO LOSS (LU)", corr: "CORRELATION",
    balance: "BALANCE (dB)", dbtpL: "TP L (dBTP)", dbtpR: "TP R (dBTP)",
  };
  return map[id] || id;
}
function unit(mode) { return mode === "program" ? " LUFS" : " dB"; }
function fmt(v, id) {
  if (v === null || v === undefined || !isFinite(v)) return "--.-";
  return v.toFixed(1) + (id === "doseN" || id === "doseO" ? "%" : "");
}
const GAUGE =
  `<div class="gauge" id="g-ID"><div class="track"></div>` +
  `<div class="band" id="gb-ID"></div><div class="dot" id="gd-ID"></div></div>`;
function makeTiles(mode) {
  const set = TILES[mode] || TILES.acoustic;
  MODE = mode;
  IDS = set.big.concat(set.small);
  const big = document.getElementById("bigrow");
  const small = document.getElementById("gridrow");
  big.innerHTML = "";
  small.innerHTML = "";
  for (const id of set.big)
    big.insertAdjacentHTML("beforeend",
      `<div class="tile"><div class="cap" id="cap-${id}"></div>` +
      `<div class="val" id="val-${id}">--.-</div>` +
      GAUGE.replaceAll("ID", id) + `</div>`);
  for (const id of set.small)
    small.insertAdjacentHTML("beforeend",
      `<div class="tile"><div class="cap" id="cap-${id}"></div>` +
      `<div class="val small" id="val-${id}">--.-</div>` +
      GAUGE.replaceAll("ID", id) + `</div>`);
}
makeTiles("acoustic");

// Peloton-style target gauges: band = target range, dot = current value
// (green in band, amber outside; track spans one band-width either side).
function setGauges(m, targets) {
  for (const id of IDS) {
    const g = document.getElementById("g-" + id);
    const t = targets ? targets[id] : null;
    if (!t) { g.style.display = "none"; continue; }
    g.style.display = "block";
    const pad = t.hi_db - t.lo_db;
    const lo = t.lo_db - pad, hi = t.hi_db + pad;
    const pct = v => Math.max(0, Math.min(100, (v - lo) / (hi - lo) * 100));
    const band = document.getElementById("gb-" + id);
    band.style.left = pct(t.lo_db) + "%";
    band.style.width = (pct(t.hi_db) - pct(t.lo_db)) + "%";
    const dot = document.getElementById("gd-" + id);
    const v = m[id];
    if (v === null || v === undefined || !isFinite(v)) {
      dot.style.display = "none";
      continue;
    }
    dot.style.display = "block";
    dot.style.left = pct(v) + "%";
    dot.className = "dot" + (v >= t.lo_db && v <= t.hi_db ? "" : " out");
  }
}

// Signal loss. The tab title is rewritten too: a dashboard left minimised on
// someone's phone is the likeliest place this gets noticed.
const BASE_TITLE = document.title;
function setSignalUi(sig, timeMs) {
  const el = document.getElementById("signalbanner");
  if (!sig || sig.state === "ok" || !sig.enabled) {
    el.className = "";
    document.title = BASE_TITLE;
    return;
  }
  const dead = sig.last_audio_ms ? Math.max(0, timeMs - sig.last_audio_ms) : 0;
  const secs = Math.round(dead / 1000);
  el.className = "on";
  el.textContent =
    (sig.state === "black" ? "NO AUDIO (digital black)" : "SILENT") +
    " — " + secs + "s";
  document.title = "⚠ NO AUDIO " + secs + "s — " + BASE_TITLE;
}

function setAlarmUi(alarm) {
  const banner = document.getElementById("alarmbanner");
  banner.className = "";
  for (const id of IDS) {
    const el = document.getElementById("val-" + id);
    if (!el) continue;
    el.classList.remove("warn", "alert");
    if (alarm && alarm.enabled && alarm.metric === id && alarm.state > 0)
      el.classList.add(alarm.state >= 2 ? "alert" : "warn");
  }
  if (alarm && alarm.enabled && alarm.state > 0) {
    banner.className = alarm.state >= 2 ? "alert" : "warn";
    banner.textContent = (alarm.state >= 2 ? "ALERT — " : "WARNING — ") +
      caption(alarm.metric, "A", MODE) + " over " +
      (alarm.state >= 2 ? alarm.alert_db : alarm.warn_db).toFixed(1) +
      unit(MODE);
  }
}

const canvas = document.getElementById("rta");
function drawRta(d) {
  const dpr = window.devicePixelRatio || 1;
  const cssW = canvas.clientWidth, cssH = canvas.clientHeight;
  if (canvas.width !== cssW * dpr) { canvas.width = cssW * dpr; canvas.height = cssH * dpr; }
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cssW, cssH);
  const bands = d.bands_db || [], centers = d.centers_hz || [];
  const yMax = d.cal_db, yMin = d.cal_db - 80;
  const left = 34, bottom = 18, top = 6, right = 6;
  const w = cssW - left - right, h = cssH - top - bottom;
  const css = getComputedStyle(document.documentElement);
  const yOf = v => top + h * (1 - (v - yMin) / (yMax - yMin));
  ctx.font = "10px ui-monospace, monospace";
  ctx.fillStyle = css.getPropertyValue("--dim");
  ctx.strokeStyle = css.getPropertyValue("--grid");
  for (let g = Math.ceil(yMin / 10) * 10; g <= yMax; g += 10) {
    const y = yOf(g);
    ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(left + w, y); ctx.stroke();
    ctx.textAlign = "right"; ctx.textBaseline = "middle";
    ctx.fillText(g.toFixed(0), left - 4, y);
  }
  const slot = w / bands.length;
  const labels = { 31.5: "31", 63: "63", 125: "125", 250: "250", 500: "500",
                   1000: "1k", 2000: "2k", 4000: "4k", 8000: "8k", 16000: "16k" };
  ctx.textAlign = "center"; ctx.textBaseline = "top";
  for (let i = 0; i < bands.length; i++) {
    if (labels[centers[i]])
      ctx.fillText(labels[centers[i]], left + (i + 0.5) * slot, top + h + 4);
    if (bands[i] === null || !isFinite(bands[i])) continue;
    const v = Math.min(yMax, Math.max(yMin, bands[i]));
    const y = yOf(v);
    ctx.fillStyle = css.getPropertyValue("--bar");
    ctx.fillRect(left + i * slot + 1, y, Math.max(1, slot - 2), top + h - y);
    ctx.fillStyle = css.getPropertyValue("--bartop");
    ctx.fillRect(left + i * slot + 1, y, Math.max(1, slot - 2), 2);
    ctx.fillStyle = css.getPropertyValue("--dim");
  }
  const peaks = d.peaks_db;
  if (peaks) {
    ctx.fillStyle = css.getPropertyValue("--warn");
    for (let i = 0; i < peaks.length; i++) {
      if (peaks[i] === null || !isFinite(peaks[i])) continue;
      const v = Math.min(yMax, Math.max(yMin, peaks[i]));
      ctx.fillRect(left + i * slot + 1, yOf(v), Math.max(1, slot - 2), 2);
    }
  }
}

const status = document.getElementById("status");
function connect() {
  const ws = new WebSocket("ws://" + location.host + "/api/stream");
  ws.onopen = () => { status.className = "live"; status.textContent = "live"; };
  ws.onclose = () => {
    status.className = "down"; status.textContent = "reconnecting…";
    setTimeout(connect, 2000);
  };
  ws.onmessage = ev => {
    const d = JSON.parse(ev.data);
    if (d.type !== "levels") return;
    const mode = d.mode || "acoustic";
    if (mode !== MODE) makeTiles(mode);
    const w = d.weighting || "A", m = d.metrics || {};
    for (const id of IDS) {
      document.getElementById("cap-" + id).textContent = caption(id, w, mode);
      document.getElementById("val-" + id).textContent = fmt(m[id], id);
    }
    setSignalUi(d.signal, d.time_ms);
    setAlarmUi(d.alarm);
    setGauges(m, d.targets);
    drawRta(d);
    document.getElementById("rtacap").textContent =
      "RTA — 1/3 OCTAVE (" + w + "-WEIGHTED, " +
      (mode === "program" ? "dBFS" : "dB SPL") + ")";
    const L = d.loudness;
    document.getElementById("modeinfo").textContent =
      mode === "program" && L
        ? "program · target " + L.target_lufs.toFixed(1) + " LUFS · ceiling " +
          L.ceiling_dbtp.toFixed(1) + " dBTP"
        : mode === "program" ? "program" : "acoustic · dB SPL";
  };
}
connect();
</script>
</body>
</html>
)HTML";
