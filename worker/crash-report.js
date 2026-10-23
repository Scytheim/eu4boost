// eu4boost crash report relay (Cloudflare Worker, free plan).
//
// The plugin POSTs a JSON report (src/report.c, schema 1) to /report. This Worker checks it, removes
// Windows profile paths once more and files it as an issue in a private GitHub repository: a new
// issue per signature and plugin version, a comment for every repeat. The GitHub token lives only
// here, as the encrypted secret GITHUB_TOKEN (fine-grained token: Issues read and write on that one
// repository); the plugin never sees it. Nothing is stored here and IP addresses are not kept.
//
// Settings (Worker -> Settings -> Variables and Secrets):
//   GITHUB_TOKEN  secret   the token
//   REPO          text     owner/name of the private repository, e.g. Scytheim/eu4boost-crash-reports

const MAX_BODY = 256 * 1024;          // the plugin sends ~100-200 KB at most
const MAX_FIELD = 128 * 1024;
const GH_LIMIT = 60000;               // GitHub allows 65536 characters per issue body or comment
const KINDS = { crash: '크래시', hang: '멈춤', exit: '비정상 종료', slow: '느림' };

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method !== 'POST' || url.pathname !== '/report') return reply({ ok: false, error: 'not found' }, 404);
    if (!env.GITHUB_TOKEN || !env.REPO) return reply({ ok: false, error: 'not configured' }, 500);
    if (limited(request.headers.get('CF-Connecting-IP') || '')) return reply({ ok: false, error: 'too many' }, 429);
    const raw = await request.text();
    if (raw.length > MAX_BODY) return reply({ ok: false, error: 'too large' }, 413);
    let r;
    try { r = JSON.parse(raw); } catch { return reply({ ok: false, error: 'bad json' }, 400); }
    const bad = check(r);
    if (bad) return reply({ ok: false, error: bad }, 400);
    for (const k of Object.keys(r)) if (typeof r[k] === 'string') r[k] = scrub(r[k]);
    if (r.pc && typeof r.pc === 'object') for (const k of Object.keys(r.pc)) if (typeof r.pc[k] === 'string') r.pc[k] = scrub(r.pc[k]);

    const sig = oneLine(r.signature).slice(0, 150);
    const title = `[${KINDS[r.kind]}] ${sig} · ${r.plugin_version}`;
    try {
      const gh = github(env);
      const old = await findIssue(gh, env.REPO, title);
      if (old) {
        if (old.comments < 200) await gh('POST', `/repos/${env.REPO}/issues/${old.number}/comments`, { body: repeatBody(r) });
        return reply({ ok: true, issue: old.number, repeat: true });
      }
      const issue = await gh('POST', `/repos/${env.REPO}/issues`, { title, body: issueBody(r) });
      for (const body of logComments(r)) await gh('POST', `/repos/${env.REPO}/issues/${issue.number}/comments`, { body });
      return reply({ ok: true, issue: issue.number, repeat: false });
    } catch (e) {
      return reply({ ok: false, error: 'github ' + (e && e.message) }, 502);
    }
  },
};

// ---- checks ----
function check(r) {
  if (!r || typeof r !== 'object' || Array.isArray(r)) return 'not an object';
  if (r.schema !== 1) return 'schema';
  if (!Object.prototype.hasOwnProperty.call(KINDS, r.kind)) return 'kind';
  if (typeof r.signature !== 'string' || !r.signature || r.signature.length > 300) return 'signature';
  if (typeof r.plugin_version !== 'string' || !/^[0-9A-Za-z.+-]{1,32}$/.test(r.plugin_version)) return 'version';
  if (typeof r.report !== 'string' || !r.report.startsWith('eu4boost crash report')) return 'report';
  for (const [k, v] of Object.entries(r)) {
    if (typeof v === 'string' && v.length > MAX_FIELD) return 'field ' + k;
    if (v !== null && typeof v === 'object' && k !== 'pc') return 'field ' + k;
  }
  return null;
}
// a few reports per minute per address (per Worker instance; the address is not stored anywhere)
const seen = new Map();
function limited(ip) {
  const now = Date.now(), e = seen.get(ip);
  if (!e || now - e.t > 60000) { seen.set(ip, { t: now, n: 1 }); if (seen.size > 5000) seen.clear(); return false; }
  return ++e.n > 6;
}
function scrub(s) {
  return s
    .replace(/[A-Za-z]:[\\/]+Users[\\/]+[^\\/\r\n"*<>|:]+/gi, '%USERPROFILE%')
    .replace(/^LaunchArguments:.*$/gim, 'LaunchArguments: <removed>')
    .replace(/(session[-_]?token|user[-_]?id|access[-_]token|auth-token|password|steamid)(["'=: ]*)[^"'\s,]+/gi, '$1$2<removed>');
}
const oneLine = (s) => String(s || '').replace(/[\r\n`]+/g, ' ').trim();
const fence = (s) => '~~~~text\n' + String(s || '').replace(/~~~~/g, '~ ~ ~ ~').replace(/@/g, '@\u200b') + '\n~~~~\n';
const clip = (s, n) => (s && s.length > n ? '…(앞부분 생략)\n' + s.slice(s.length - n) : s || '');
const head = (s, n) => (s && s.length > n ? s.slice(0, n) + '\n…(뒷부분 생략)' : s || '');

// ---- GitHub ----
function github(env) {
  return async (method, path, body) => {
    const res = await fetch('https://api.github.com' + path, {
      method,
      headers: {
        Authorization: `Bearer ${env.GITHUB_TOKEN}`,
        Accept: 'application/vnd.github+json',
        'X-GitHub-Api-Version': '2022-11-28',
        'User-Agent': 'eu4boost-crash-relay',
        ...(body ? { 'Content-Type': 'application/json' } : {}),
      },
      body: body ? JSON.stringify(body) : undefined,
    });
    if (!res.ok) throw new Error(String(res.status));
    return res.status === 204 ? null : res.json();
  };
}
async function findIssue(gh, repo, title) {     // the open issue with exactly this title (newest 300)
  for (let page = 1; page <= 3; page++) {
    const list = await gh('GET', `/repos/${repo}/issues?state=open&per_page=100&page=${page}`);
    const hit = list.find((i) => !i.pull_request && i.title === title);
    if (hit) return hit;
    if (list.length < 100) return null;
  }
  return null;
}

// ---- text ----
function summary(r) {
  const pc = r.pc || {};
  return [
    `**종류:** ${KINDS[r.kind]} · **플러그인:** ${oneLine(r.plugin_version)} · **보고 ID:** ${oneLine(r.id)}`,
    `**서명:** \`${oneLine(r.signature)}\``,
    `**PC:** ${oneLine(pc.cpu)} · ${pc.threads | 0}스레드 · RAM ${pc.ram_mb | 0} MB · ${oneLine(pc.gpu)} · Windows ${oneLine(pc.windows)} · ${oneLine(pc.locale)}`,
    `**설정:** \`${oneLine(r.config).slice(0, 600)}\``,
    `**DLL:** \`${oneLine(r.dll_sha256).slice(0, 16)}\` · **EU4dll:** \`${oneLine(r.eu4dll_sha256).slice(0, 16)}\``,
    ...(r.saves ? [`**세이브:** ${oneLine(r.saves)}`] : []),
  ].join('\n');
}
function issueBody(r) {
  let s = summary(r) + '\n\n### 보고서\n' + fence(head(r.report, 30000));
  s += '\n### 모드·DLC (dlc_load.json)\n' + fence(head(r.dlc_load, 3000));
  if (r.game_exception || r.game_meta)
    s += '\n### 게임 크래시 정보 (exception.txt, meta.yml)\n' + fence(head(r.game_exception, 2500) + '\n\n' + head(r.game_meta, 2500));
  s += '\n### plugins 폴더\n' + fence(head(r.plugins_dir, 2000));
  return head(s, GH_LIMIT);
}
function repeatBody(r) {
  return head(`### 다시 발생\n${summary(r)}\n\n` + fence(head(r.report, 40000)), GH_LIMIT);
}
function logComments(r) {
  const out = [];
  if (r.plugin_log) out.push(head('### 플러그인 로그 (마지막 부분)\n' + fence(clip(r.plugin_log, 55000)), GH_LIMIT));
  const g = [];
  if (r.system_log) g.push('#### system.log (하드웨어 줄)\n' + fence(head(r.system_log, 6000)));
  if (r.game_log) g.push('#### game.log (마지막 부분)\n' + fence(clip(r.game_log, 20000)));
  if (r.error_log) g.push('#### error.log (마지막 부분)\n' + fence(clip(r.error_log, 25000)));
  if (g.length) out.push(head('### 게임 로그\n' + g.join('\n'), GH_LIMIT));
  return out;
}
function reply(obj, status) {
  return new Response(JSON.stringify(obj), { status, headers: { 'Content-Type': 'application/json' } });
}
