#pragma once

// The player picker served at "/". The browser downloads Sleeper's player list directly
// (api.sleeper.app sends Access-Control-Allow-Origin: *) and keeps a trimmed copy in localStorage
// for a day, so the board only ever receives the chosen IDs and labels.

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Scoreboard</title>
<style>
  :root { --bg: #111; --fg: #eee; --dim: #888; --line: #333; --accent: #ffd78c; --ok: #50ff78; }
  body { background: var(--bg); color: var(--fg); font: 16px/1.4 system-ui, sans-serif; margin: 0 auto; max-width: 560px; padding: 16px; }
  h1 { font-size: 20px; margin: 0 0 4px; }
  .meta { color: var(--dim); font-size: 14px; margin-bottom: 16px; }
  input, select, button { font: inherit; color: var(--fg); background: #1c1c1c; border: 1px solid var(--line); border-radius: 6px; padding: 8px; }
  button { cursor: pointer; }
  button.primary { background: var(--accent); color: #111; border-color: var(--accent); font-weight: 600; }
  #search { width: 100%; box-sizing: border-box; }
  ul { list-style: none; margin: 0; padding: 0; }
  #results li { padding: 8px; border-bottom: 1px solid var(--line); cursor: pointer; }
  #results li:hover { background: #1c1c1c; }
  #roster li { display: flex; gap: 8px; align-items: center; padding: 6px 0; border-bottom: 1px solid var(--line); }
  #roster .who { flex: 1; min-width: 0; }
  #roster .label { width: 9ch; }
  #roster .game { font-size: 12px; }
  #roster .pts { width: 5ch; text-align: right; color: var(--accent); font-variant-numeric: tabular-nums; }
  .pos { display: inline-block; width: 4.2ch; font-size: 12px; font-weight: 700; }
  .QB { color: #ff4646; } .RB { color: #3cdc5a; } .WR { color: #4696ff; } .TE { color: #ffa028; } .K { color: #c86eff; } .DEF { color: #a0a0a0; }
  .row { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; margin: 16px 0; }
  .dim { color: var(--dim); }
  #msg { min-height: 1.4em; }
</style>
</head>
<body>
<h1>Scoreboard</h1>
<div class="meta" id="meta">Loading...</div>

<h2 style="font-size:16px">On the panel (<span id="count">0</span>/9), top to bottom</h2>
<ul id="roster"></ul>

<div class="row">
  <label>Scoring
    <select id="scoring">
      <option value="pts_ppr">PPR</option>
      <option value="pts_half_ppr">Half PPR</option>
      <option value="pts_std">Standard</option>
    </select>
  </label>
  <label>Brightness <input id="brightness" type="range" min="5" max="255"></label>
  <button class="primary" id="save">Save to panel</button>
</div>
<div class="row" id="tests" style="display:flex;flex-wrap:wrap;gap:6px;margin-top:8px">
  <span class="dim" style="align-self:center">Test celebration:</span>
  <button data-kind="0">Rush</button>
  <button data-kind="1">Catch</button>
  <button data-kind="2">Pass</button>
  <button data-kind="3">Defense</button>
  <button data-kind="4">Kick</button>
  <button data-kind="5">Rushing TD</button>
  <button data-kind="6">Receiving TD</button>
  <button data-kind="7">Passing TD</button>
  <button data-kind="8">Defensive TD</button>
  <button data-kind="9">Other</button>
</div>
<div id="msg" class="dim"></div>

<h2 style="font-size:16px">Add players</h2>
<input id="search" placeholder="Loading player list..." disabled autocomplete="off">
<ul id="results"></ul>
<p class="dim" id="listinfo"></p>

<script>
const MAX = 9;
const CACHE_KEY = 'sleeper_players_v1';
const CACHE_MS = 24 * 3600 * 1000;           // Sleeper asks for the player list at most once a day
const POSITIONS = ['QB', 'RB', 'WR', 'TE', 'K', 'DEF'];
let players = [];                            // [{id, name, pos, team, key}]
let roster = [];                             // [{id, label, pos, team, pts}]
let dirty = false;

const $ = id => document.getElementById(id);
const norm = s => s.toLowerCase().replace(/[^a-z0-9]/g, '');

function el(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  Object.assign(e, attrs || {});
  for (const k of kids) e.append(k);
  return e;
}

function defaultLabel(p) {
  if (p.pos === 'DEF') return p.team + ' D';
  const parts = p.name.split(' ').filter(w => !/^(jr\.?|sr\.?|ii|iii|iv|v)$/i.test(w));
  return parts[parts.length - 1] || p.name;
}

// Sleeper's /v1/players/nfl, filtered per position so each reply is small, trimmed to what the
// picker needs.
async function loadPlayers() {
  try {
    const c = JSON.parse(localStorage.getItem(CACHE_KEY) || 'null');
    if (c && Date.now() - c.t < CACHE_MS && Array.isArray(c.list)) return c;
  } catch (e) {}
  const replies = await Promise.all(POSITIONS.map(pos =>
    fetch('https://api.sleeper.app/v1/players/nfl?active=true&position=' + pos).then(r => {
      if (!r.ok) throw new Error('Sleeper HTTP ' + r.status);
      return r.json();
    })));
  const list = [];
  for (const map of replies) {
    for (const p of Object.values(map)) {
      if (!p.team) continue;                 // free agents score nothing
      const name = p.full_name || [p.first_name, p.last_name].filter(Boolean).join(' ');
      list.push([p.player_id, name, p.position, p.team]);
    }
  }
  const c = { t: Date.now(), list };
  try { localStorage.setItem(CACHE_KEY, JSON.stringify(c)); } catch (e) {}
  return c;
}

function renderRoster() {
  $('count').textContent = roster.length;
  const ul = $('roster');
  ul.replaceChildren();
  if (!roster.length) ul.append(el('li', { className: 'dim', textContent: 'No players yet.' }));
  roster.forEach((p, i) => {
    const label = el('input', { className: 'label', value: p.label, maxLength: 15, title: 'Name on the panel; if too long, letters are left out from the middle (mostly vowels). Shown in full on the update screen' });
    label.oninput = () => { p.label = label.value; setDirty(); };
    const up = el('button', { textContent: '↑', title: 'Move up', disabled: i === 0 });
    up.onclick = () => { [roster[i - 1], roster[i]] = [roster[i], roster[i - 1]]; setDirty(); renderRoster(); };
    const down = el('button', { textContent: '\u2193', title: 'Move down', disabled: i === roster.length - 1 });
    down.onclick = () => { [roster[i + 1], roster[i]] = [roster[i], roster[i + 1]]; setDirty(); renderRoster(); };
    const rm = el('button', { textContent: '✕', title: 'Remove' });
    rm.onclick = () => { roster.splice(i, 1); setDirty(); renderRoster(); renderResults(); };
    const who = el('span', { className: 'who' },
      el('span', { className: 'pos ' + p.pos, textContent: p.pos }), (p.name || p.id) + ' ',
      el('span', { className: 'dim', textContent: p.team }));
    if (p.game) who.append(el('div', { className: 'game dim', textContent: p.game }));
    const pts = el('span', { className: 'pts', textContent: p.pts == null ? '-' : p.pts.toFixed(1) });
    ul.append(el('li', {}, who, label, pts, up, down, rm));
  });
}

function renderResults() {
  const q = norm($('search').value);
  const ul = $('results');
  ul.replaceChildren();
  if (q.length < 2) return;
  const taken = new Set(roster.map(p => p.id));
  const hits = players.filter(p => p.key.includes(q) && !taken.has(p.id)).slice(0, 20);
  for (const p of hits) {
    const li = el('li', {}, el('span', { className: 'pos ' + p.pos, textContent: p.pos }), p.name + ' ',
      el('span', { className: 'dim', textContent: p.team }));
    li.onclick = () => {
      if (roster.length >= MAX) { $('msg').textContent = 'The panel shows at most 9 players.'; return; }
      roster.push({ id: p.id, label: defaultLabel(p), pos: p.pos, team: p.team, name: p.name, pts: null });
      $('search').value = '';
      setDirty(); renderRoster(); renderResults();
    };
    ul.append(li);
  }
  if (!hits.length) ul.append(el('li', { className: 'dim', textContent: 'No match.' }));
}

function setDirty() { dirty = true; $('msg').textContent = 'Unsaved changes.'; }

function nameFor(id) {
  const p = players.find(p => p.id === id);
  return p ? p.name : '';
}

function applyServer(cfg, replaceRoster) {
  const age = cfg.last_ok_s < 0 ? 'no update yet' : 'updated ' + cfg.last_ok_s + ' s ago';
  $('meta').textContent = cfg.season ? `${cfg.season} ${cfg.season_type} week ${cfg.week} · ${age}` +
    (cfg.status && cfg.status !== 200 ? ` · last error ${cfg.status}` : '') : 'Panel is starting...';
  if (replaceRoster) {
    roster = cfg.players.map(p => ({ ...p, name: nameFor(p.id) }));
    $('scoring').value = cfg.scoring;
    $('brightness').value = cfg.brightness;
  } else {
    for (const p of roster) {
      const s = cfg.players.find(q => q.id === p.id);
      if (s) { p.pts = s.pts; p.game = s.game; }
    }
  }
  renderRoster();
}

async function refresh(replaceRoster) {
  try {
    const r = await fetch('/api/config');
    applyServer(await r.json(), replaceRoster && !dirty);
  } catch (e) { $('meta').textContent = 'Panel not reachable.'; }
}

$('save').onclick = async () => {
  const body = {
    scoring: $('scoring').value,
    brightness: +$('brightness').value,
    players: roster.map(({ id, label, pos, team }) => ({ id, label: label.trim() || id, pos, team })),
  };
  $('msg').textContent = 'Saving...';
  try {
    const r = await fetch('/api/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    if (!r.ok) throw new Error(await r.text());
    dirty = false;
    applyServer(await r.json(), true);
    $('msg').textContent = 'Saved. Points refresh within a few seconds.';
  } catch (e) { $('msg').textContent = 'Save failed: ' + e.message; }
};
$('scoring').onchange = setDirty;
$('brightness').onchange = setDirty;
document.querySelectorAll('#tests button').forEach(b => b.onclick = async () => {
  try {
    const r = await fetch('/api/test?kind=' + b.dataset.kind, { method: 'POST' });
    $('msg').textContent = r.ok ? b.textContent + ' celebration queued: watch the panel.' : 'Test failed.';
  } catch (e) { $('msg').textContent = 'Panel not reachable.'; }
});
$('search').oninput = renderResults;

(async () => {
  await refresh(true);
  try {
    const c = await loadPlayers();
    players = c.list.map(([id, name, pos, team]) => ({ id, name, pos, team, key: norm(name + ' ' + team) }));
    $('search').disabled = false;
    $('search').placeholder = 'Search by name or team';
    $('listinfo').textContent = `${players.length} players from Sleeper, list fetched ${new Date(c.t).toLocaleString()}.`;
    for (const p of roster) p.name = p.name || nameFor(p.id);
    renderRoster();
  } catch (e) {
    $('search').placeholder = 'Player list failed to load';
    $('listinfo').textContent = 'Could not load the Sleeper player list: ' + e.message + '. The phone needs internet access.';
  }
  setInterval(() => refresh(false), 30000);
})();
</script>
</body>
</html>
)HTML";
