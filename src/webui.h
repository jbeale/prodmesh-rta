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
  #alarmbanner {
    display: none; margin-bottom: 12px; padding: 8px 14px; border-radius: 6px;
    font-weight: 700; font-size: 15px; letter-spacing: 0.5px;
  }
  #alarmbanner.warn { display: block; background: #4a4020; color: var(--warn); }
  #alarmbanner.alert { display: block; background: #4a2020; color: var(--alert); }
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
</header>
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
const BIG = ["laf", "las", "leq"];
const SMALL = ["leqS", "leqL", "lzpk", "lcpk", "ca", "l10", "l50", "l90",
               "doseN", "doseO"];
function caption(id, w, m) {
  const map = {
    laf: "L" + w + "F (FAST)", las: "L" + w + "S (SLOW)", leq: "L" + w + "eq",
    leqS: "LAeq SHORT", leqL: "LAeq LONG", lzpk: "LZpk", lcpk: "LCpk",
    ca: "C-A RATIO", l10: "L10", l50: "L50", l90: "L90",
    doseN: "DOSE NIOSH", doseO: "DOSE OSHA",
  };
  return map[id] || id;
}
function fmt(v, id) {
  if (v === null || v === undefined || !isFinite(v)) return "--.-";
  return v.toFixed(1) + (id === "doseN" || id === "doseO" ? "%" : "");
}
function makeTiles() {
  for (const id of BIG)
    document.getElementById("bigrow").insertAdjacentHTML("beforeend",
      `<div class="tile"><div class="cap" id="cap-${id}"></div>` +
      `<div class="val" id="val-${id}">--.-</div></div>`);
  for (const id of SMALL)
    document.getElementById("gridrow").insertAdjacentHTML("beforeend",
      `<div class="tile"><div class="cap" id="cap-${id}"></div>` +
      `<div class="val small" id="val-${id}">--.-</div></div>`);
}
makeTiles();

function setAlarmUi(alarm) {
  const banner = document.getElementById("alarmbanner");
  banner.className = "";
  for (const id of BIG.concat(SMALL)) {
    const el = document.getElementById("val-" + id);
    el.classList.remove("warn", "alert");
    if (alarm && alarm.enabled && alarm.metric === id && alarm.state > 0)
      el.classList.add(alarm.state >= 2 ? "alert" : "warn");
  }
  if (alarm && alarm.enabled && alarm.state > 0) {
    banner.className = alarm.state >= 2 ? "alert" : "warn";
    banner.textContent = (alarm.state >= 2 ? "ALERT — " : "WARNING — ") +
      caption(alarm.metric, "A") + " over " +
      (alarm.state >= 2 ? alarm.alert_db : alarm.warn_db).toFixed(1) + " dB";
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
    const w = d.weighting || "A", m = d.metrics || {};
    for (const id of BIG.concat(SMALL)) {
      document.getElementById("cap-" + id).textContent = caption(id, w, m);
      document.getElementById("val-" + id).textContent = fmt(m[id], id);
    }
    setAlarmUi(d.alarm);
    drawRta(d);
    document.getElementById("rtacap").textContent =
      "RTA — 1/3 OCTAVE (" + w + "-WEIGHTED, dB SPL)";
  };
}
connect();
</script>
</body>
</html>
)HTML";
