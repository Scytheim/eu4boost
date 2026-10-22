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
- 다음 일: HANDOFF.md §9(0번: 커밋·기본값은 사용자 결정 대기).

## 사용자
- 한국어로 짧고 쉽게 설명한다. 사용자가 해야 할 일은 번호 목록으로 준다(예: 세이브 1596.09.10 로드 → 5배속 1년 → 종료 → "끝"이라고 알려 주기). 사용자가 원하면 Claude가 컴퓨터 사용으로 직접 측정한다(10-15부터).
- PC: Ryzen 5 9600X(6C/12T), Windows. 게임: `<게임 폴더>`. 분석 자료: `<작업 폴더>`.

## 명령
```sh
sh build.sh                   # → build/eu4boost.dll. Windows(Git Bash): <도구 폴더>\llvm-mingw-* 자동 사용, WSL/Linux: clang + lld + mingw-w64
sh test/run_tests.sh          # 스모크 테스트 13개 (build.sh 먼저). Windows는 바로 실행, Linux는 Wine. 끝 줄 "all 13 cases passed"
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
- **사용자의 원본 세이브(GER.eu4 등)는 절대 쓰지 않는다.** 모드 세이브 측정은 복사본 eb_GER로만 하고, 실행마다 `<작업 폴더>\save_backup_20261015\GER.eu4`에서 되돌린다(아이언맨은 불러온 이름으로 자동 저장). eu4.exe를 PowerShell로 직접 켜지 않는다(런처 사용).
- **게임 로직(시뮬레이션 결과)에 영향을 줄 수 있는 변경은 반드시 검증한다**: `configs/validation_seed1234_checksum.ini`(fixed_seed=1234 + state_checksum=1)로 test.eu4를 **콘솔 없이**(이미 관전 상태로 저장됨) 400일쯤 → `compare_state.py`로 `validation/baseline_seed1234_20261015.csv`와 모든 날이 IDENTICAL이어야 한다. 콘솔 명령을 치면 체크섬이 달라진다(HANDOFF §7). 단, 이 게임은 실행 속도에 따라 결과가 조금 달라져서 속도를 크게 바꾸는 개선은 고정 시드 비교가 갈라질 수 있다 → 호출마다 원래 함수와 비교하는 verify 모드 + 시간을 되돌린 실행으로 검증한다(HANDOFF §7 한계 2).
- 속도 판단: 같은 설정 두 번도 2~3% 차이가 난다. 작은 개선은 `[parallel] stats` 루프별 통계나 프로파일로 판단하고, 콘솔 timer_dump만으로 판단하지 않는다(날짜 사이 시간을 빼고 재기 때문).
- ini는 UTF-16LE + CRLF로 유지한다. DLL 교체는 게임이 꺼져 있을 때만 하고, 복사 후 해시를 확인한다.
- 바꾼 동작이 있으면 ini 키로 켜고 끌 수 있게 하고, 기본값은 안전한 쪽으로 둔다. 실험 기능은 기본 꺼짐.
- 버전 문자열은 `src/common.h`의 EU4BOOST_VERSION. 배포할 때 dist/README_KO.md와 configs를 함께 갱신한다.
- 서드파티: mimalloc v2.2.4(MIT), MinHook(BSD-2) — third_party/에 라이선스 포함. 새 외부 코드는 사용자 승인 후 추가한다.
- git 저장소다(브랜치 main, 2026-10-15 시작). `.gitattributes`로 모든 파일을 바이트 그대로 보관한다(소스는 LF, ini는 UTF-16LE + CRLF).
- 빌드 도구: llvm-mingw 20261006 msvcrt판(LLVM 23.1.3)을 `<도구 폴더>\llvm-mingw-20261006-msvcrt-x86_64`에 풀어 둠. 게임에 깔린 dev3까지는 클라우드의 clang 18.1.3 빌드다(같은 소스, msvcrt.dll 사용은 같음).
