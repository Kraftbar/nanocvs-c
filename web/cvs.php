<?php
const NANOCVS = '/home/nybo/services/nanocvs-c/nanocvs-c';
const DB = '/home/nybo/services/nanocvs-c/nanocvs.db';

// clean up stale WAL/SHM owned by www-data when WAL is empty/gone
$wal = DB.'-wal';
$shm = DB.'-shm';
$wal_empty = !file_exists($wal) || filesize($wal) === 0;
if ($wal_empty) {
    if (file_exists($wal) && is_writable($wal)) @unlink($wal);
    if (file_exists($shm) && is_writable($shm)) @unlink($shm);
}

function h(string $s): string {
    return htmlspecialchars($s, ENT_QUOTES, 'UTF-8');
}

function fmt_ts(string $iso): string {
    try {
        $dt = new DateTime($iso, new DateTimeZone('UTC'));
        $dt->setTimezone(new DateTimeZone('Europe/Oslo'));
        return $dt->format('Y-m-d H:i:s');
    } catch (Exception $e) {
        return str_replace(['T','Z'], [' ',''], substr($iso, 0, 19));
    }
}

function run(array $args): string {
    $cmd = array_map('escapeshellarg', array_merge([NANOCVS], $args, ['--ro']));
    return shell_exec(implode(' ', $cmd) . ' 2>&1') ?? '';
}

$path  = $_GET['path'] ?? '';
$rev   = $_GET['rev'] ?? '';
$limit = max(1, min(500, (int)($_GET['n'] ?? 50)));

// --- data ---
$status = json_decode(run(['status', '--json']), true) ?: [];
$roots  = array_filter(explode("\n", trim(run(['roots']))));
$week   = trim(run(['week', '--days', '7']));

// heatmap: fetch last 500 events, group by dir, score by recency
$heat_events = json_decode(run(['log', '--limit', '500', '--json']), true) ?: [];
$heat = [];
$now  = time();
foreach ($heat_events as $e) {
    $p    = $e['relpath'];
    $ts   = strtotime($e['ts']) ?: 0;
    $segs = explode('/', $p);
    $dir  = count($segs) >= 2 ? $segs[0].'/'.$segs[1] : $segs[0];
    if (!isset($heat[$dir])) $heat[$dir] = ['count' => 0, 'latest' => 0];
    $heat[$dir]['count']++;
    if ($ts > $heat[$dir]['latest']) $heat[$dir]['latest'] = $ts;
}
foreach ($heat as &$h) {
    $age_days = ($now - $h['latest']) / 86400;
    $h['score'] = $h['count'] * max(0.1, 1 - $age_days / 30);
}
unset($h);
uasort($heat, fn($a, $b) => $b['score'] <=> $a['score']);

// 24h chart — bucket heat_events by hour (0 = most recent)
$chart = array_fill(0, 24, ['added' => 0, 'deleted' => 0]);
foreach ($heat_events as $e) {
    $age_h = (int)(($now - (strtotime($e['ts']) ?: 0)) / 3600);
    if ($age_h < 0 || $age_h >= 24) continue;
    $chart[$age_h]['added']   += (int)$e['lines_added'];
    $chart[$age_h]['deleted'] += (int)$e['lines_deleted'];
}
$chart_max = max(1, max(array_map(fn($b) => max($b['added'], $b['deleted']), $chart)));

$log_args = ['log', '--limit', (string)$limit, '--json'];
$rows = json_decode(run($log_args), true) ?: [];
if ($path) {
    $rows = array_values(array_filter($rows, fn($r) =>
        $r['relpath'] === $path || str_starts_with($r['relpath'], $path . '/')
    ));
}

// diff inline
$diff_html = '';
if ($rev && $path) {
    $out = run(['diff', $path, $rev]);
    $parts = [];
    foreach (explode("\n", $out) as $line) {
        if (str_starts_with($line, '+++') || str_starts_with($line, '---'))
            $parts[] = '<span style="color:#888">' . h($line) . "</span>";
        elseif (str_starts_with($line, '@@'))
            $parts[] = '<span style="color:#06c">' . h($line) . "</span>";
        elseif (str_starts_with($line, '+'))
            $parts[] = '<span style="color:#080">' . h($line) . "</span>";
        elseif (str_starts_with($line, '-'))
            $parts[] = '<span style="color:#c00">' . h($line) . "</span>";
        else
            $parts[] = h($line);
    }
    $diff_html = implode("\n", $parts);
}

?><!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>nanocvs</title>
<style>
body { font: 14px/1.6 sans-serif; max-width: 1400px; margin: 2rem auto; padding: 0 1rem; color: #222; }
.page { display: flex; gap: 2rem; align-items: flex-start; }
.main { flex: 1 1 0; min-width: 0; }
.side { flex: 0 0 280px; position: sticky; top: 1rem; }
.log-wrap { display: flex; gap: 1.5rem; align-items: flex-start; }
.log-wrap > div { flex: 1 1 0; min-width: 0; overflow-x: auto; }
.diff-panel { flex: 0 0 45%; }
.diff-panel h2 { margin-top: 0; }
h2 { font-size: .8rem; color: #999; text-transform: uppercase; letter-spacing: .05em; margin: 2rem 0 .5rem; border-bottom: 1px solid #eee; padding-bottom: .3rem; }
.stat-row { display: flex; gap: 1rem; flex-wrap: wrap; margin-bottom: .5rem; }
.stat { border: 1px solid #eee; padding: .4rem .7rem; }
.stat .l { font-size: 11px; color: #999; }
.stat .v { }
ul.roots { list-style: none; padding: 0; margin: 0; font-size: 13px; }
ul.roots li { padding: .15rem 0; color: #555; }
pre.week { font: 12px/1.5 monospace; background: #f8f8f8; border: 1px solid #eee; padding: .7rem 1rem; margin: 0; }
form { display: flex; gap: .4rem; align-items: center; flex-wrap: wrap; }
input[type=text] { border: 1px solid #ccc; padding: .2rem .4rem; font: inherit; width: 260px; }
input[type=number] { border: 1px solid #ccc; padding: .2rem .4rem; font: inherit; width: 60px; }
button { border: 1px solid #ccc; padding: .2rem .6rem; font: inherit; cursor: pointer; background: #f5f5f5; }
a.clr { color: #aaa; font-size: 12px; }
table { width: 100%; border-collapse: collapse; font-size: 13px; margin-top: .5rem; }
th { text-align: left; border-bottom: 2px solid #ddd; padding: .3rem .5rem; color: #888; font-weight: normal; }
td { padding: .3rem .5rem; border-bottom: 1px solid #f0f0f0; vertical-align: top; }
tr:hover td { background: #fafafa; }
.c { color: #aaa; }
.create { color: #080; }
.modify { color: #840; }
.delete { color: #c00; }
.add { color: #080; }
.del { color: #c00; }
.dim { color: #aaa; }
a { color: #06c; }
a.plain { color: inherit; text-decoration: none; }
a.plain:hover { text-decoration: underline; }
pre.diff { font: 12px/1.5 monospace; background: #f8f8f8; border: 1px solid #eee; padding: 1rem; overflow-x: auto; }
.heat-row { display: flex; align-items: center; gap: .5rem; margin-bottom: .35rem; font-size: 12px; }
.heat-bar { height: 10px; border-radius: 2px; min-width: 3px; }
.heat-label { color: #555; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; flex: 1; cursor: pointer; }
.heat-label:hover { color: #06c; }
.heat-count { color: #aaa; font-size: 11px; flex: 0 0 auto; }
</style>
</head>
<body>

<h2 style="margin-top:0">lines changed &mdash; past 24h <span style="font-weight:normal;color:#aaa;font-size:11px;text-transform:none;letter-spacing:0"><span style="color:#4c9">&#9632;</span> added &nbsp;<span style="color:#e55">&#9632;</span> deleted</span></h2>
<svg viewBox="0 0 960 90" style="width:100%;display:block;margin-bottom:1.5rem" preserveAspectRatio="none">
  <line x1="0" y1="50" x2="960" y2="50" stroke="#eee" stroke-width="1"/>
  <?php for ($i = 0; $i < 24; $i++):
    $sx = (23 - $i) * 40 + 6;
    $ah = (int)round($chart[$i]['added']   / $chart_max * 45);
    $dh = (int)round($chart[$i]['deleted'] / $chart_max * 45);
  ?>
  <?php if ($ah > 0): ?><rect x="<?= $sx ?>" y="<?= 50 - $ah ?>" width="28" height="<?= $ah ?>" fill="#4c9" opacity="0.85"/><?php endif; ?>
  <?php if ($dh > 0): ?><rect x="<?= $sx ?>" y="50" width="28" height="<?= $dh ?>" fill="#e55" opacity="0.85"/><?php endif; ?>
  <?php endfor; ?>
  <text x="4"   y="88" font-size="10" fill="#bbb" font-family="sans-serif">24h ago</text>
  <text x="956" y="88" font-size="10" fill="#bbb" font-family="sans-serif" text-anchor="end">now</text>
</svg>

<div class="page">
<div class="main">

<h2>change log</h2>
<form method="get">
  <input type="text" name="path" placeholder="filter by path" value="<?= h($path) ?>">
  <input type="number" name="n" value="<?= $limit ?>" min="1" max="500">
  <button type="submit">filter</button>
  <?php if ($path): ?><a class="clr" href="?">clear</a><?php endif; ?>
</form>

<div class="log-wrap">
<div>
<?php if (empty($rows)): ?>
<p style="color:#aaa;margin-top:.5rem">no events</p>
<?php else: ?>
<table>
<tr><th>#</th><th>time</th><th>action</th><th>path</th><th>lines</th><th>+/-</th><th>bytes</th><th></th></tr>
<?php foreach ($rows as $r):
    $id     = (string)$r['id'];
    $lines  = ($r['lines_before'] ?? '-') . '->' . ($r['lines_after'] ?? '-');
    $bytes  = ($r['bytes_before'] ?? '-') . '->' . ($r['bytes_after'] ?? '-');
    $delta  = '<span class="add">+' . $r['lines_added'] . '</span> <span class="del">-' . $r['lines_deleted'] . '</span>';
    $ts     = fmt_ts($r['ts']);
    $isActive = ($id === $rev && $r['relpath'] === $path);
?>
<tr<?= $isActive ? ' style="background:#fffbe6"' : '' ?>>
  <td class="c"><?= h($id) ?></td>
  <td class="dim" style="white-space:nowrap"><?= h($ts) ?></td>
  <td class="<?= h($r['action']) ?>"><?= h($r['action']) ?></td>
  <td><a class="plain" href="?path=<?= urlencode($r['relpath']) ?>&n=<?= $limit ?>"><?= h($r['relpath']) ?></a></td>
  <td class="dim"><?= h($lines) ?></td>
  <td><?= $delta ?></td>
  <td class="dim"><?= h($bytes) ?></td>
  <td><?php if ($r['action'] === 'modify'): ?>
    <a href="?path=<?= urlencode($r['relpath']) ?>&rev=<?= urlencode($id) ?>&n=<?= $limit ?>" style="font-size:11px;color:#aaa">diff</a>
  <?php endif; ?></td>
</tr>
<?php endforeach; ?>
</table>
<?php endif; ?>
</div>

<?php if ($diff_html): ?>
<div class="diff-panel">
  <h2>diff &mdash; <?= h($path) ?> @ <?= h($rev) ?> <a href="?path=<?= urlencode($path) ?>&n=<?= $limit ?>" style="font-size:11px;font-weight:normal">close</a></h2>
  <pre class="diff"><?= $diff_html ?></pre>
</div>
<?php endif; ?>
</div>

</div><!-- .main -->
<div class="side">
  <h2>status</h2>
  <div class="stat-row" style="flex-direction:column;gap:.4rem">
  <?php foreach (['tracked paths' => $status['tracked_paths'] ?? null, 'existing now' => $status['existing_now'] ?? null, 'total changes' => $status['total_changes'] ?? null] as $k => $v): ?>
  <div class="stat" style="width:100%"><div class="l"><?= h($k) ?></div><div class="v"><?= h((string)$v) ?></div></div>
  <?php endforeach; ?>
  <?php $ls = $status['last_scan'] ?? null; if ($ls): ?>
  <div class="stat" style="width:100%">
    <div class="l">last scan</div>
    <div style="font-size:12px;margin-top:.2rem;display:flex;flex-direction:column;gap:.1rem">
      <span><span style="color:#aaa">id</span> <?= h((string)$ls['id']) ?></span>
      <span><span style="color:#aaa">started</span> <?= h(fmt_ts($ls['started_at'])) ?></span>
      <?php if (!empty($ls['finished_at'])): ?><span><span style="color:#aaa">finished</span> <?= h(fmt_ts($ls['finished_at'])) ?></span><?php endif; ?>
      <span><span style="color:#aaa">files</span> <?= h((string)$ls['files_seen']) ?></span>
      <span><span style="color:#aaa">changes</span> <?= h((string)$ls['changes_found']) ?></span>
    </div>
  </div>
  <?php endif; ?>
  </div>

  <h2>activity heat</h2>
  <?php
  $max_score = max(array_column($heat, 'score') ?: [1]);
  foreach ($heat as $dir => $h):
      $pct   = (int)(100 * $h['score'] / $max_score);
      $color = '#999';
      $age   = round(($now - $h['latest']) / 3600);
      $age_s = $age < 24 ? "{$age}h ago" : round($age/24)."d ago";
  ?>
  <div class="heat-row" onclick="document.querySelector('input[name=path]').value='<?= h(addslashes($dir)) ?>';document.querySelector('input[name=n]').value=Math.max(<?= $h['count'] ?>,document.querySelector('input[name=n]').value);document.querySelector('form').submit()">
    <div class="heat-bar" style="width:<?= $pct ?>%;background:<?= $color ?>"></div>
    <span class="heat-label" title="<?= h($dir) ?>"><?= h($dir) ?></span>
    <span class="heat-count"><?= $h['count'] ?> · <?= $age_s ?></span>
  </div>
  <?php endforeach; ?>

  <h2>tracked roots</h2>
  <ul class="roots">
  <?php foreach ($roots as $r): ?>
  <li><?= h($r) ?></li>
  <?php endforeach; ?>
  </ul>

  <h2>last 7 days</h2>
  <pre class="week" style="font-size:11px"><?= h($week) ?></pre>
</div>
</div><!-- .page -->

</body>
</html>
