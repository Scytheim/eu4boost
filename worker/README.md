# 크래시 보고 중간 서버 (Cloudflare Worker)

플러그인이 보낸 크래시 보고를 받아 GitHub **비공개** 저장소에 이슈로 올립니다. GitHub 토큰은 이 Worker의 암호화된 비밀값으로만 보관되고 플러그인 DLL에는 들어가지 않습니다. Worker는 보고를 저장하지 않고 IP도 기록하지 않습니다.

- 코드: [crash-report.js](crash-report.js)
- 시험: `node test/worker_test.mjs` (가짜 GitHub로 확인, 저장소 루트에서 실행)
- 비용: Cloudflare Workers 무료 플랜(하루 10만 요청)과 GitHub 무료 비공개 저장소만 씁니다. 결제 수단은 등록하지 않습니다. 무료 한도를 넘으면 요청이 실패할 뿐 요금이 청구되지 않습니다.

## 설치 (한 번만)

1. **비공개 저장소 만들기**: GitHub에서 New repository → 이름 `eu4boost-crash-reports` → **Private** → Create.
2. **토큰 만들기**: GitHub → Settings → Developer settings → Personal access tokens → **Fine-grained tokens** → Generate new token.
   - Repository access: **Only select repositories** → `eu4boost-crash-reports`
   - Permissions → Repository permissions → **Issues: Read and write** (Metadata: Read-only는 자동으로 붙음)
   - Expiration: 원하는 기간(최대 1년). 만료되면 보고가 실패하고, 플러그인은 보고를 PC에 남겨 두었다가 다음 실행 때 다시 보냅니다. 만료 전에 새 토큰으로 바꿔 넣으면 됩니다.
   - 만든 토큰(`github_pat_...`)은 이 단계에서만 보입니다. 다음 단계에 바로 붙여 넣고 다른 곳에는 저장하지 마세요.
3. **Worker 만들기**: Cloudflare 가입(무료) → Workers & Pages → Create → Worker → 이름 `eu4boost-report` → Deploy.
   - Edit code → 기본 코드를 지우고 `crash-report.js` 내용을 붙여 넣기 → Deploy.
4. **설정 넣기**: 그 Worker → Settings → Variables and Secrets → Add
   - Type **Secret**, Name `GITHUB_TOKEN`, Value = 2번의 토큰
   - Type **Text**, Name `REPO`, Value = `Scytheim/eu4boost-crash-reports`
   - Deploy(저장).
5. **주소 확인**: Worker 화면의 주소 `https://eu4boost-report.<계정 이름>.workers.dev`에 `/report`를 붙인 것이 보고 주소입니다. 이 주소를 플러그인의 `EB_REPORT_URL`(src/report.c, 빌드 때 `-DEB_REPORT_URL=L"..."`)에 넣고 릴리스합니다.

## 운영

- 같은 서명·같은 플러그인 버전의 보고는 열린 이슈에 댓글로 붙습니다. 이슈를 닫으면 다음 보고부터 새 이슈가 됩니다.
- 보관 기간 약속(해결 뒤 또는 1년 뒤 삭제): 오래된 이슈는 저장소 관리자가 지웁니다.
  ```sh
  gh issue list --repo Scytheim/eu4boost-crash-reports --state all --search "created:<2027-10-23" --json number --jq ".[].number"
  gh issue delete <번호> --repo Scytheim/eu4boost-crash-reports --yes
  ```
- 가짜 보고가 많이 오면 Worker를 지우거나 이름을 바꾸면 됩니다(토큰은 이 저장소의 이슈만 다룰 수 있어 코드나 다른 저장소는 건드릴 수 없습니다). 한 주소에서 1분에 6개를 넘게 보내면 거절합니다.
