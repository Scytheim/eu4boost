// Tests worker/crash-report.js with a fake GitHub API: node test/worker_test.mjs [payload.json]
// (payload: a report the plugin sent, e.g. crash_reports\sent\*.json; a small built-in one otherwise)
import fs from 'node:fs';
import worker from '../worker/crash-report.js';

const env = { GITHUB_TOKEN: 'test-token', REPO: 'owner/reports' };
const issues = [];
const calls = [];
globalThis.fetch = async (url, init) => {
  const u = new URL(url), body = init.body ? JSON.parse(init.body) : null;
  calls.push({ method: init.method, path: u.pathname + u.search, body });
  if (init.headers.Authorization !== 'Bearer test-token') throw new Error('token missing');
  if (init.method === 'GET' && u.pathname === '/repos/owner/reports/issues')
    return new Response(JSON.stringify(issues), { status: 200 });
  if (init.method === 'POST' && u.pathname === '/repos/owner/reports/issues') {
    const it = { number: issues.length + 1, title: body.title, body: body.body, comments: 0 };
    issues.push(it);
    return new Response(JSON.stringify(it), { status: 201 });
  }
  const m = u.pathname.match(/^\/repos\/owner\/reports\/issues\/(\d+)\/comments$/);
  if (init.method === 'POST' && m) { issues[m[1] - 1].comments++; return new Response('{}', { status: 201 }); }
  return new Response('{}', { status: 404 });
};

const sample = process.argv[2] ? JSON.parse(fs.readFileSync(process.argv[2], 'utf8')) : {
  schema: 1, id: '20261023_101010_crash', kind: 'crash', signature: 'EXCEPTION_ACCESS_VIOLATION eu4.exe+0x8e8da5',
  plugin_version: '0.4.1-dev', config: 'allocator=1', report: 'eu4boost crash report\r\nkind: crash\r\nC:\\Users\\Someone\\Documents\\x @someone ~~~~',
  plugin_log: 'log', pc: { cpu: 'CPU', threads: 12, ram_mb: 32000, gpu: 'GPU', windows: '10.0.26300', locale: 'ko-KR' },
};
let fails = 0;
const ok = (c, what) => { console.log((c ? 'ok   ' : 'FAIL ') + what); if (!c) fails++; };
const post = (obj, ip = '1.2.3.4', path = '/report') => worker.fetch(new Request('https://relay.example' + path, {
  method: 'POST', headers: { 'CF-Connecting-IP': ip, 'Content-Type': 'application/json' }, body: typeof obj === 'string' ? obj : JSON.stringify(obj) }), env);

let r = await post(sample), j = await r.json();
ok(r.status === 200 && j.ok && j.issue === 1 && !j.repeat, `first report -> issue 1 (${r.status} ${JSON.stringify(j)})`);
const first = issues[0];
ok(first && first.title.startsWith('[크래시] ') && first.title.endsWith(' · ' + sample.plugin_version), 'title: ' + (first && first.title));
ok(first && first.body.length <= 60000 && !/Users[\\/]+Someone/i.test(JSON.stringify(calls)), 'body within GitHub limit, profile path removed');
ok(first && !first.body.includes('@someone'), 'mentions neutralized');
r = await post(sample, '5.6.7.8'); j = await r.json();
ok(j.ok && j.issue === 1 && j.repeat && issues.length === 1 && issues[0].comments >= 1, 'same signature again -> comment on issue 1');
r = await post({ ...sample, kind: 'hang', signature: 'HANG eu4.exe+0x1234' }, '9.9.9.9'); j = await r.json();
ok(j.ok && j.issue === 2 && issues[1].title.startsWith('[멈춤] '), 'other signature -> issue 2');
r = await post({ ...sample, kind: 'exit', signature: 'ABNORMAL_EXIT', game_meta: 'LaunchArguments: --paradox-account-userid="abc123" --pdx-launcher-session-token="SECRET9"\nMods: x',
                 error_log: 'login userid=987654 password: hunter2 sessionToken="tok"' }, '9.9.9.8'); j = await r.json();
const exitText = JSON.stringify(calls.filter((c) => c.body).map((c) => c.body));
ok(j.ok && !/abc123|SECRET9|987654|hunter2|"tok/.test(exitText), 'account ids, tokens and passwords removed');
r = await post({ ...sample, kind: 'slow', signature: 'SLOW stall eu4.exe+0x75d690', saves: '140 files, 900.0 MB (largest 40.0 MB)' }, '9.9.9.7'); j = await r.json();
const slowIssue = issues.find((i) => i.number === j.issue);
ok(j.ok && slowIssue && slowIssue.title.startsWith('[느림] ') && slowIssue.body.includes('**세이브:** 140 files'), 'slow report -> [느림] issue with the save count');
ok((await post('{bad', '2.2.2.2')).status === 400, 'bad JSON refused');
ok((await post({ ...sample, schema: 2 }, '2.2.2.3')).status === 400, 'wrong schema refused');
ok((await post({ ...sample, report: 'x' }, '2.2.2.4')).status === 400, 'report text without its header refused');
ok((await post({ ...sample, plugin_log: 'x'.repeat(300000) }, '2.2.2.5')).status === 413, 'oversized body refused');
ok((await post(sample, '2.2.2.6', '/other')).status === 404, 'other path -> 404');
let last;
for (let i = 0; i < 8; i++) last = await post(sample, '3.3.3.3');
ok(last.status === 429, 'more than 6 reports a minute from one address -> 429');
console.log(fails ? `${fails} FAILED` : 'worker: all checks passed');
process.exit(fails ? 1 : 0);
