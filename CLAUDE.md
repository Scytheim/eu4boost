# eu4boost — Europa Universalis IV 1.37.5.0 성능 플러그인

EU4dll 플러그인 로더(`plugins\*.dll`)로 게임 시작 직후 로드되는 x64 DLL이다. eu4.exe 파일은 고치지 않고 메모리에서만 후킹(MinHook)·IAT 교체·데이터 패치를 한다.
전체 맥락: **docs/HANDOFF.md**(반드시 먼저 읽기), 시간순 상세 기록: docs/ANALYSIS_LOG.md.

## 현재 상태 (2026-10-15)
- 0.2 기능으로 원래 게임 대비 실제 속도 1.32배(1년 진행 21.3초 → 16.2초).
- **0.3.0 정식판**(병렬 for 개선 기본 켬)이 PC에 설치돼 있다(10-15, llvm-mingw 빌드, `<작업 폴더>\eu4boost_v0.3.0.zip`). 일반 모드 A/B: 같은 536일에서 23.31 → 23.98일/초(−2.8%). 고정 시드 검증: 실험 끔과 켬이 441일 완전 동일.
- **0.4.0-dev1**(10-15 밤, `save-compress` 브랜치, 커밋 전)이 설치돼 있다. 모드 아이언맨 세이브(짭짭죽, eb_GER 1543)에서: 세이브 848 → 405ms(`[save]`), modifier 합치기 −7.6%(`[modifiers] fast_add`), 한 프레임 여러 날(`[render] speed5_days_per_frame`, 기본 꺼짐) 2일 −3.4% / 3일 −5.2%.
- 국가 업데이트 병렬화 실험(10-15 밤, 10-16 보정): `[country] parallel_refresh`(RefreshKnowledge 병렬 미리 계산, 기본 꺼짐) −1.7%. 보정 후 원래 시점 재계산과 99.9% 동일, 경합 검사 0. 사용자는 본 게임에서 켜고 씀. 국가 루프는 하루의 25%지만 대부분 난수를 써서 순서를 바꿀 수 없음.
- 5배속 중간 렌더 간격 권장값을 200 → 50(원래값)으로(10-16): 중간 프레임이 입력도 읽어서 200이면 스페이스바가 늦음. 비용은 월초 약 17ms(전체 0.5% 안팎, 잡음 범위).
- 매월 1일 처리 분석(10-16): 1일 약 320ms지만 국가·프로빈스 월간 처리 모두 난수를 순서대로 써서 큰 개선 불가. modifier 작은 합치기 SIMD 검색(`[modifiers] fast_search`, 기본 켬)으로 메인 스레드 약 0.5%만 줄임.
- 세이브 나라 정보 병렬 쓰기(10-17): `[save] parallel_countries`(권장 ini 1) 저장 410 → 245ms, 같은 날 저장 파일 해시 동일, 두 캠페인 29번 저장 바이트 비교 차이 0. 시험용 두 번째 세이브 복사본은 `<작업 폴더>\save_backup_20261017\V2_1562.eu4`(사용자 GER_V2_Backup_Backup, 프로이센 1562.10.27).
- 100년 끔/켬 측정(10-17, 외부 측정 프로그램 `<작업 폴더>\bench\eu4bench.py`, test.eu4 1600→1700 관전, 플레이세트 "기본", 3회씩): 36.18 → 27.67분(1.31배), 메인 스레드 0.89 → 0.96코어, 게임 메모리 +0.4GB. 결과 `bench\results\REPORT_KO.md`.
- 실 플레이용 정리(10-17): 권장 ini에서 speed_log·profiler 끔, modifier·국가 통계는 stats=1일 때만. 사용자 게임 폴더는 log=0까지 끔(권장 ini는 문제 확인용으로 log=1 유지).
- 10-18: 철인모드 콘솔 단축키(`[console] ironman_hotkey`, Ctrl+Alt+C, ini는 게임 시작 때만 읽음 — 사용자의 켜기/끄기 bat이 이 값을 바꾸고, 사용자가 1초마다 읽는 방식은 원하지 않음). 플레이 중 크래시 2건(게임 버그: 해제된 modifier 출처를 읽는 툴팁, 합병 때 외교 관계 누락) → `[guard] tooltip/relation`(기본 켬). 덤프 분석은 `tools/analysis/crash_stack.py`. 사용자 게임 ini는 log=1로 되돌림.
- 10-19: 메인 스레드 시간 지도 재측정(ANALYSIS_LOG 10-19). 10-18의 툴팁 장치가 월간 프로빈스 계산에서 메인의 66%를 써서 2~3배 느려졌던 것을 찾아 고침(하루 계산 중엔 건너뜀 + VEH 읽기 검사). 디스패치 비용은 1% 이하로 측정됨 → AI 유닛 루프의 가장 긴 항목(메인 대기 6.4%)이 더 큰 대상.
- 10-21: AI 루프 분석(사용자 본 게임 1874년 복사본 `save_backup_20261021\GER_1874.eu4`). 부대 AI 떼어 내기(`ai_split`)는 효과 없음. 프로빈스 평가 점수 미리 계산(`ai_prov_split`, 실험 기본 꺼짐): 같은 실행 번갈아 비교 큰 평가 90.6 → 26.8ms, 검증 18만 번 일치. 남은 큰 지연은 외교 장관(최대 2.6초).
- 10-22: 외교 장관 평화 협상안의 AE 계산(`ai_ae_cache`, 실험 기본 꺼짐): AI 루프의 잠금 걸린 공유 AE 맵 대신 스레드별 표를 쓰고 AE 합계를 C로 다시 짬. 합계마다 번갈아 비교 61.0 → 27.6µs(2.2배), 검증 합계 1만 6천 번 일치. 큰 협상은 실행마다 나오기도 하고 안 나오기도 해서 날 단위 비교로는 잴 수 없음.
- 10-22: 후반부 시간 지도(두 실험 켬): 하루 88.7ms 중 AI 루프 21%, 월간 25%, 나라 직렬 10%, 렌더 9%. 부대 AI 이동 검사(`ai_army_fast`, 실험 기본 꺼짐): 플래그 먼저 + 부대 능력 0xe0을 AI 작업 실행 동안 기억. 검사 0.339 → 0.085µs(4배), 검증 7,900만 번 일치.
- 10-22: 매월 프로빈스 설명 문장 재사용(`[modifiers] prov_text_cache`, guard.c의 hk_tip, 배포 설정 1): 문장 하나 82 → 2.65µs, 매월 1일 약 190ms 단축, 검증 13만 7천 번 일치. 측정 때 런처 플레이 세트를 짭짭죽으로 바꿨다가 기본으로 되돌림(사용자는 GER_Test를 기본 세트로 플레이 중).
- 10-22 밤: 후반부 전체 측정(1874.07 → 1880.01, `bench\results\late_1022`): 플러그인 없음 4.00분, 기본 2.92분(1.37배), 실험 5개 켬 2.71분(1.48배). 새 시간 지도(`profile_1022b`): AI 루프 15%, 나라 직렬 12%, 월간 21%(MonthlyUpdateSerial 7%, 프로빈스 월간 6%: CalculateModifier 3.4%·제도 전파 3.3%), 첩보망 등 5%, 렌더 10%.
- 10-23: 사용자 불편(저장 창·불러오기 화면·삭제가 느림) 원인 = EU4dll용 0 채우기가 세이브 목록의 68MB 할당마다 0을 씀 → `[allocator] zero_fill_windows_kb`로 수정(목록 73.9 → 7.2ms). 온라인 제한은 날짜가 넘어갈 때만(`[online] only_while_running`). 크래시 보고(`[report]`, src/crash.c·report.c, worker/crash-report.js): 기본 주소 EB_REPORT_URL은 `https://eu4boost-report.eu4boost.workers.dev/report`(Worker 코드를 바꾸면 사용자가 Cloudflare에 다시 붙여 넣는다). 진단용 `[general] ui_timing`. 느려짐 보고(`[report] slow`: 목록 0.3초, 멈칫 2초, 불러오기 90초) + 불러오기 시간 로그 `load:`. 배포 전 확인: 후반부 진행 0.4.0 2.45분 vs 0.4.1 2.36분, 불러오기 기능 끔 19.2초 vs 0.4.1 17.0초(평균). 0.4.1 배포. 크래시 보고 저장소 `Scytheim/eu4boost-crash-reports`는 이 PC의 gh(GitHub CLI, winget 설치, PATH에 없으면 기본 설치 위치)로 읽는다. 테스트가 끝나면 게임 폴더 ini의 `ab_test=0` 확인.
- 10-23 오후: 실제 사용자 보고 2건(ANALYSIS_LOG). 크래시 = 느려짐 보고가 다시 돌린 메인 스레드의 스택을 읽다 난 것 → 0.4.2-dev에서 다 읽을 때까지 세워 두고 오류 막이(eb_guard_call) 안에서 읽음, 보고서에 모드 이름 목록 추가. 느림 = 세이브 895개(7.4GB)를 저장 창을 열 때마다 하나씩 여는 데(CreateFileW, 한 개 5.4ms, 백신 검사 추정) 5.6초. 테스트 15개.
- 10-23 밤: 메모리 조사 1차(진단 [diag] mem_sites, 게임 자체 증가는 후반부 1년에 약 10MB). 9번 보고(세이브 구조 손상) 검토 → 저장 기능 세 가지는 원인이 될 수 없음, 그래도 나라 바이트 구조 검사(balanced)·저장 기록(saves.txt)·이전 세션 로그 확인을 넣음(저장 22번 검사 실패 0, +6ms). 남은 일: 플러그인 쪽 메모리 증가(5.4년 +339MB vs 없음 +113MB), Anbennar(윾타지) 호환 확인, 0.4.2 배포.
- 10-23 밤: 자동 업데이트(src/update.c, [update] auto=1 기본, 다음 실행부터, ECDSA P-256 서명 확인, 서명 도구 tools/sign.c, 비밀 키는 <작업 폴더>\keys\). 릴리스 파일은 zip 하나(DLL은 무압축 저장, 서명은 zip 주석 — release.sh가 만들고 다시 읽어 확인).
- 10-23 밤: **0.4.2 배포**(릴리스는 zip 하나, 업데이트가 zip에서 DLL·서명을 읽음). 남은 일: 플러그인 쪽 메모리 증가, Anbennar(윾타지) 호환 확인, 0.4.1 사용자의 불러오기 크래시 보고 추적.
- 10-24: 이슈 4~21 검토. 18번(Anbennar 크래시) = 기념물 정의 안의 has_construction이 대형 사업 배열이 두 배로 늘 때 옛 주소를 들고 있는 게임 버그 → `[guard] great_project`(기본 켬, 불러오기 때만 동작). 윾타지 플레이세트로 실제 확인(트리거 2개를 3번 옮김, 정상 종료). 0.4.3 배포는 사용자 결정 대기. 다음 후보: 세이브 목록 캐시(20번, 저장 창 20초), 플러그인 쪽 메모리 증가, 이슈 제목 버전.
- 10-24: 세이브 목록 캐시(`[save] list_cache`, src/savelist.c, 기본 켬): 47개 목록 1,328 → 34ms, 항목 비교 차이 0, 망가진 캐시는 원래 세이브로 다시 읽음. 비압축 세이브가 느린 원인 = 게임 meta 읽기가 "}" 뒤 키를 놓쳐 첫 프로빈스까지 약 3MB를 한 글자씩 읽음. 게임 재시작(싱글 화면 "뒤로") 때 비정상 종료 오보고도 고침(session.txt pid). 남은 일: 메모리 증가, 스크립트 변수(19번), 그다음 0.4.3 배포.
- 10-25: 스크립트 변수 읽기(`[script] fast_variables`, src/script.c, 기본 켬): MEIOU(사용자가 구독, 시험 플레이세트 eb_MEIOU)에서 1월 2일 멈춤 8.7 → 6.8초, 4년 −7.8%, 11억 번 비교 차이 0. 남은 일: 메모리 증가(mimalloc 주기 정리 1.7%도 같이), 그다음 0.4.3 배포.
- 10-25: **0.4.3 배포**(공개 91ed4b5, 릴리스 자산 zip 하나, 설명 비움). 0.4.2 사용자는 자동 업데이트로 받음. 남은 일: 메모리 증가(mimalloc 주기 정리 1.7%도 같이), 보고 제목의 버전 표시(17번).
- 10-25: 24번 크래시(병렬 RefreshKnowledge가 작업 스레드에서 휴전 끝 알림을 만듦) 수정: 작업 스레드의 알림을 모아 메인 스레드에서 내보냄(src/country.c, 시험용 `[country] test_truce_end`/`test_no_defer`). 재현(이전 방식 3일 만에 크래시)과 수정 확인, 속도 차이 없음.
- 10-25: 클릭 없는 게임 시험(사용자 요청: 컴퓨터 사용 없이 반복 작업 자동화). `[test] autorun_until`(src/autorun.c, 시험 전용, ini 파일에 없음) + `<작업 폴더>\bench\auto\run.sh 이름 DLL late|ger|세이브 종료일 [ini 키...]`: eu4.exe -skiplauncher -continuelastsave를 최소화로 켜고(continue_game.json을 eb_auto로 바꿨다 되돌림), 이어하기의 도전 과제 경고를 건너뛰고, 불러온 뒤 5배속, 종료일에 WM_CLOSE로 정상 종료. 결과는 `bench\auto\runs\이름\run.txt`. 끝나면 `restore.sh`로 플레이 DLL·ini를 되돌린다. 앞으로 반복 시험(배포 전 확인, 재현 시험)은 이것으로 하고 컴퓨터 사용은 화면을 봐야 할 때만.
- 10-25: **0.4.4 배포**(24번 수정 + 시험용 자동 진행, 공개 66c3a86, 릴리스 자산 zip 하나, 설명 비움). 배포 DLL 확인은 클릭 없는 자동 시험으로 함. 남은 일: 메모리 증가(mimalloc 주기 정리 1.7%도 같이), 보고 제목의 버전 표시(17번), 11번(d3d9 크래시)이 24번과 같은 원인인지 0.4.4 뒤 보고로 확인.
- 10-25: 17번(보고 제목 버전: 그 세션의 버전, 보낸 버전은 sender_version)과 22번(전쟁 참가 멈춤: `[country] parallel_war`, 참가국 RefreshKnowledge를 묶음별 병렬, 그룹·종주국/속국 순서 유지, 시험용 `test_war_all`) 수정. 검증 차이 0, 큰 시험 4.3배.
- 10-25: **0.4.5 배포**(17번·22번, 공개 c526b62, 릴리스 자산 zip 하나, 설명 비움, 배포 DLL 확인은 자동 시험). 멀티플레이 개선은 사용자 요청으로 계획에서 뺌. 다음: 29번(전쟁 창의 평화 제안 계산 멈춤).
- 10-25: 비정상 종료 진단(Windows 앱 로그 Application Error 1000을 다음 실행 때 찾아 모듈·예외 코드·위치, 자기 TerminateProcess는 호출자 기록, src/crash.c)과 29번(전쟁 창 멈춤: 메인 스레드의 협상안 채우기·수락도 한 번 = AE 표 한 세대 + 땅 탐색 FUN_14036eb10 결과 기억, src/aiprof.c, `[parallel] ai_ae_cache`와 함께, 시험용 `[test] warview`). 1874 시험 96개 화면 4.6 → 1.8초, 검증 차이 0.
- 10-25: **0.4.6 배포**(29번·비정상 종료 진단, 공개 8bf8305, 릴리스 자산 zip 하나, 설명 비움, 배포 DLL 확인은 자동 시험). 29번의 남은 부분: 채우기의 땅 덩어리 수 세기(FUN_1409ace20 → FUN_1409a30b0, 협상안을 읽음)와 요구 가능 검사(FUN_14097a1c0).
- 10-25: 업데이트 결과 보고(0.4.7용, 커밋 전): 업데이터가 `crash_reports\update.txt`에 결과를 남기고 다음 실행이 `[업데이트] UPDATE_OK`(이전 → 새 버전) 또는 `UPDATE_FAILED <이유>`(한 번만)를 보냄([report] upload=1, 버전·결과만). Worker 코드도 바뀜(사용자가 Cloudflare에 붙여 넣어야 함). 가짜 서버로 성공·실패 실제 확인. 0.4.1 사용자는 자동 업데이트가 없어 직접 받아야 함(다운로드 수 차이의 대부분).
- 10-25: 36번(0.4.6 첫 크래시) = 한글 철인모드 게임 이름: EU4dll은 일반 저장 이름만 UTF-8로 바꾸고 철인모드 경로(FUN_1405ca070)는 안 바꿔 PhysFS가 거부 → 게임이 일부러 예외. `[guard] ironman_name`(기본 1)이 EU4dll과 같은 방식으로 UTF-8 이름을 줌. 재현(수정 끔 = 같은 서명 크래시)과 수정(eb_시험.eu4 저장) 실제 확인.
- 다음 일: HANDOFF.md §9(0번: 커밋·기본값은 사용자 결정 대기).

## 사용자
- 한국어로 짧고 쉽게 설명한다. 사용자가 해야 할 일은 번호 목록으로 준다(예: 세이브 1596.09.10 로드 → 5배속 1년 → 종료 → "끝"이라고 알려 주기). 사용자가 원하면 Claude가 컴퓨터 사용으로 직접 측정한다(10-15부터).
- PC: Ryzen 5 9600X(6C/12T), Windows. 게임: `<게임 폴더>`. 분석 자료: `<작업 폴더>`.

## 명령
```sh
sh build.sh                   # → build/eu4boost.dll. Windows(Git Bash): <도구 폴더>\llvm-mingw-* 자동 사용, WSL/Linux: clang + lld + mingw-w64
sh test/run_tests.sh          # 스모크 테스트 16개 (build.sh 먼저). Windows는 바로 실행, Linux는 Wine. 끝 줄 "all 16 cases passed"
node test/worker_test.mjs     # 크래시 보고 중간 서버(worker/crash-report.js)를 가짜 GitHub로 시험
sh tools/release.sh           # 릴리스 파일(zip, eu4boost.dll, .sha256) → build/release/v버전/ (버전에 -dev가 없어야 함)
powershell -File build_windows.ps1   # PowerShell 빌드 (build.sh와 같은 코드)
python tools/re/fn.py 14093fe30                 # Ghidra 디컴파일 보기 (EU4_DECOMP)
python tools/re/disasm.py 0x140179e90 300       # 디스어셈블 (EU4_EXE)
python tools/re/sig.py 0x179e90                 # 서명 바이트 뽑기
python tools/analysis/compare_speed.py A=a.csv B=b.csv
python tools/analysis/parallel_stats.py eu4boost_prev.log eu4boost.log
python tools/analysis/compare_state.py validation/baseline_seed1234_20261015.csv eu4boost_state_X.csv
```
- Python 패키지: `pip install pefile capstone numpy`.
- 경로는 `tools/re/re_paths.py`에 있다(환경 변수 EU4_EXPORT / EU4_DECOMP / EU4_EXE).

## 규칙
- **모든 패치는 eu4.exe TimeDateStamp 0x66FE6AB9와 바이트 서명을 확인한 뒤에만 적용한다.** 다르면 그 기능만 끄고 로그에 남긴다(크래시보다 무동작).
- 게임 함수를 새로 후킹하기 전에 EU4dll 패치 주소와 겹치는지 확인한다(게임 폴더 `pattern_eu4_jps_2.log`의 주소 90개, 그중 .text 79개. 베이스는 로그의 "change_tag" 문자열 주소 − eu4.exe 안 RVA로 구함: 로그가 다시 써지면 바뀜: 10-17 0x7ff6ef640000, 10-18 0x7ff7a0b70000). EU4dll은 malloc의 0 채우기에 의존한다(0.1.0 크래시 원인).
- 세이브 목록 순서는 사용자가 저장할 때마다 바뀐다(수정 시간순). 시작 전에 항상 툴팁의 파일 이름을 확인한다(10-17에 사용자 세이브 GER_V2를 선택한 적이 있음, 시작 전에 알아채서 바꿈).
- **사용자의 원본 세이브(GER.eu4 등)는 절대 쓰지 않는다.** 모드 세이브 측정은 복사본 eb_GER로만 하고, 실행마다 `<작업 폴더>\save_backup_20261015\GER.eu4`에서 되돌린다(아이언맨은 불러온 이름으로 자동 저장). 컴퓨터 사용으로 측정할 때는 eu4.exe를 PowerShell로 직접 켜지 않는다(창이 화면 밖에 최소화됨, 런처 사용).
- **게임 로직(시뮬레이션 결과)에 영향을 줄 수 있는 변경은 반드시 검증한다**: `configs/validation_seed1234_checksum.ini`(fixed_seed=1234 + state_checksum=1)로 test.eu4를 **콘솔 없이**(이미 관전 상태로 저장됨) 400일쯤 → `compare_state.py`로 `validation/baseline_seed1234_20261015.csv`와 모든 날이 IDENTICAL이어야 한다. 콘솔 명령을 치면 체크섬이 달라진다(HANDOFF §7). 단, 이 게임은 실행 속도에 따라 결과가 조금 달라져서 속도를 크게 바꾸는 개선은 고정 시드 비교가 갈라질 수 있다 → 호출마다 원래 함수와 비교하는 verify 모드 + 시간을 되돌린 실행으로 검증한다(HANDOFF §7 한계 2).
- 속도 판단: 같은 설정 두 번도 2~3% 차이가 난다. 작은 개선은 `[parallel] stats` 루프별 통계나 프로파일로 판단하고, 콘솔 timer_dump만으로 판단하지 않는다(날짜 사이 시간을 빼고 재기 때문).
- ini는 UTF-16LE + CRLF로 유지한다. DLL 교체는 게임이 꺼져 있을 때만 하고, 복사 후 해시를 확인한다.
- 바꾼 동작이 있으면 ini 키로 켜고 끌 수 있게 하고, 기본값은 안전한 쪽으로 둔다. 실험 기능은 기본 꺼짐.
- 버전 문자열은 `src/common.h`의 EU4BOOST_VERSION. 배포할 때 dist/README_KO.md와 configs를 함께 갱신한다.
- 서드파티: mimalloc v2.2.4(MIT), MinHook(BSD-2) — third_party/에 라이선스 포함. 새 외부 코드는 사용자 승인 후 추가한다.
- git 저장소다(브랜치 main, 2026-10-15 시작). `.gitattributes`로 모든 파일을 바이트 그대로 보관한다(소스는 LF, ini는 UTF-16LE + CRLF).
- 빌드 도구: llvm-mingw 20261006 msvcrt판(LLVM 23.1.3)을 `<도구 폴더>\llvm-mingw-20261006-msvcrt-x86_64`에 풀어 둠. 게임에 깔린 dev3까지는 클라우드의 clang 18.1.3 빌드다(같은 소스, msvcrt.dll 사용은 같음).
