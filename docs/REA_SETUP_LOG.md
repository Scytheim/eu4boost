# REA (Reverse Engineer Anything) 설치 기록

(claude.ai 프로젝트 "유로파4"의 `claude/REA 설치 기록.md` 사본, 2026-10-14 작성)

- 설치일: 2026-10-14
- PC: Windows x64, Claude 데스크톱 앱(Windows 스토어/MSIX판, 패키지 `Claude_pzs8sxrjxfjjc`)
- Node.js 24.19.0 (`C:\Program Files\nodejs`)
- REA 6.4.0, npm 전역 설치: `%APPDATA%\npm\node_modules\rea-agents`
- Ghidra 12.1.4: `<도구 폴더>\ghidra_12.1.4_PUBLIC` (SHA-256 ddac49f9…d4db 확인)
- JDK: Eclipse Temurin 21.0.12.1 zip판, `<도구 폴더>\jdk-21` (시스템 JAVA_HOME/PATH는 변경 안 함)
- REA/Ghidra 임시 폴더: `<도구 폴더>\rea-temp`
- 저장소: https://github.com/morluto/rea

## 등록 방식
Claude 앱이 실제로 읽는 설정 파일(MSIX 전용 사본)에 직접 등록함:
`%LOCALAPPDATA%\Packages\Claude_pzs8sxrjxfjjc\LocalCache\Roaming\Claude\claude_desktop_config.json`

```json
"mcpServers": { "rea": {
  "command": "C:\\Program Files\\nodejs\\node.exe",
  "args": ["%APPDATA%\\npm\\node_modules\\rea-agents\\scripts\\rea.mjs", "mcp"],
  "env": {
    "REA_ANALYSIS_PROVIDER": "ghidra",
    "GHIDRA_INSTALL_DIR": "C:\\tools\\ghidra_12.1.4_PUBLIC",
    "JAVA_HOME": "C:\\tools\\jdk-21",
    "TEMP": "C:\\tools\\rea-temp",
    "TMP": "C:\\tools\\rea-temp"
  } } }
```
백업: 같은 폴더의 `claude_desktop_config.json.before-rea.bak`(REA 등록 전), `.before-ghidra.bak`(Ghidra 설정 전).
같은 내용을 `%APPDATA%\Claude\claude_desktop_config.json`에도 맞춰 둠 (`rea doctor`가 이 파일을 봄).

## 확인 결과
- `rea doctor --provider ghidra`: 10개 항목 모두 통과 (Windows native Job Object/DACL 제어 포함).
- Claude 앱 경유 MCP 시험: `where.exe` 복사본(`ghidra_test_where.exe`)을 open_binary → binary_overview(함수 198개, 문자열 153개) → entry 디컴파일까지 정상.

## 알아둘 점
- REA 6.4.0의 `rea setup --client claude_desktop`은 MSIX판을 인식하지 못하고 `%APPDATA%\Claude`에 등록함 → 앱이 읽지 않음. MSIX 처리는 REA main 브랜치에만 있고 6.4.0에는 없음. 수정된 버전이 나오기 전에는 `rea setup`으로 Claude 앱을 다시 등록하지 말 것.
- Claude 앱이 띄우는 MCP 서버는 사용자 환경 변수를 물려받지 않으므로 Ghidra 경로는 위 `env`에 넣어야 함.
- 업데이트: `npm install -g rea-agents@<버전>` (경로가 같아서 재등록 불필요) 후 Claude 앱 완전 종료 → 재실행.
- 클라우드 세션에서는 `mcp__remote-devices__rea__*` 도구로 보임 (처음엔 RefreshMcpTools 필요할 수 있음).
- Windows Ghidra(P0)는 읽기 전용: 디컴파일/어셈블리/참조/문자열 등 조회만 가능, 이름 변경·주석 같은 편집은 불가. 대상은 로컬 NTFS의 네이티브 x86/x64 PE(exe/dll).
- 큰 exe는 첫 분석(가져오기+자동 분석)이 오래 걸릴 수 있음. eu4.exe(38MB)는 REA로 직접 열면 첫 분석이 15분 넘게 걸려 MCP 연결이 60초에 끊기고 REA가 재시작됨 → 대신 PC의 Ghidra 헤드리스(`tools/ghidra/`)로 내보낸 TSV/디컴파일 파일을 씀.
- Codex 설정(`~/.codex/config.toml`)에 이전 REA 등록이 버전 불일치 상태로 남아 있음 (미정리).

## 작업 폴더
`<작업 폴더>`에 설치/진단/수정 스크립트와 로그가 있음 (install_rea, check_rea, fix_rea, install_ghidra, set_rea_env.js).
