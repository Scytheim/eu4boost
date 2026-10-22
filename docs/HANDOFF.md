# eu4boost 인수인계 문서 (2026-10-15)

claude.ai(클라우드 세션 + 사용자 PC 연결)에서 2026-10-14~15에 한 작업 전체를 Claude Code에서 이어가기 위한 문서.
시간순 상세 기록은 [ANALYSIS_LOG.md](ANALYSIS_LOG.md), 분석 도구 설치 기록은 [REA_SETUP_LOG.md](REA_SETUP_LOG.md).

---

## 1. 한눈에 보기

**목표.** Europa Universalis IV 1.37.5.0 "Inca"(Windows x64)를 더 빠르게 만들기. 사용자가 특히 원하는 것은 멀티스레드 활용(메인 스레드 부하 분산).

**방식.** eu4.exe는 고치지 않는다. EU4dll의 플러그인 로더가 `plugins\` 폴더의 DLL을 게임 시작 직후(CRT 초기화 전)에 읽는다. 그 DLL(`eu4boost.dll`)이 메모리에서 함수 후킹(MinHook)과 IAT 교체, 데이터 패치를 한다. 모든 패치는 eu4.exe의 PE 타임스탬프와 바이트 서명을 확인한 뒤에만 적용된다.

**지금까지의 결과.** 아래는 1596.09.10 세이브, 관전(observe) 모드, 5배속으로 같은 408일 구간을 잰 값이다.

| 구성 | 실제 속도 | 원래 게임 대비 |
|---|---|---|
| 원래 게임(플러그인 기능 모두 끔) | 19.15일/초 | 기준 |
| 0.2 기능(할당기 + 메인 스레드 고정 해제 + 중간 렌더 감소 + 온라인 폴링 감소) | 25.26일/초 | **1.32배**(시간 −24%) |
| 0.3.0-dev3 병렬 개선 | 일반 모드 A/B(10-15, 같은 536일) 23.31 → 23.98일/초 | **+2.9%**(하루 약 1ms) |

**0.4.0-dev1(10-15 밤).** 사용자의 모드 아이언맨 세이브(짭짭죽 플레이세트, GER 브란덴부르크 1543.05.31, 복사본 eb_GER)로 잰 값이다.

| 개선 | 결과 |
|---|---|
| 세이브 병렬 압축 + 숫자 쓰기(`[save]`) | 아이언맨 분기 자동 저장 848 → 405ms(−52%). 세이브 내용 동일 |
| modifier 합치기 O(n)(`[modifiers] fast_add`) | 같은 754일 17.33 → 18.75일/초(**−7.6%**). 원래 함수와 32,144번 비교해 불일치 0 |
| 세이브 나라 정보 병렬 쓰기(`[save] parallel_countries`, 10-17) | 저장 1번 410 → 245ms(−40%), 분기 첫날 0.80 → 0.65초, 같은 737일 전체 −3.9%(잡음 ±2%). 같은 날 저장한 파일 SHA-256 동일, 두 캠페인 29번 저장 바이트 비교 차이 0 |
| modifier 작은 합치기 SIMD 검색(`[modifiers] fast_search`, 10-16) | 메인 스레드의 modifier 합치기 1.69 → 1.23%(약 −0.5%). 2년 동안 1,081만 번 비교해 불일치 0 |
| 5배속 한 프레임 여러 날(`[render] speed5_days_per_frame`, 기본 꺼짐) | 2일 −3.4%, 3일 −5.2%. 대신 5배속 화면 갱신 초당 약 18 → 10 / 7번 |
| RefreshKnowledge 병렬 미리 계산(`[country] parallel_refresh`, **실험, 기본 꺼짐**) | 국가 루프 12.93 → 11.91ms/일(하루 약 1.7%). 10-16 보정 후 원래 시점 재계산과 99.9% 동일(경합 0). 사용자는 본 게임에 켜고 씀 |

같은 설정을 두 번 돌려도 2~3% 차이가 난다. 그래서 작은 개선은 루프별 통계(`[parallel] stats`)로 판단한다.

**지금 PC에 설치된 상태(10-15 밤 기준)**
- DLL: **0.4.0-dev1**(`save-compress` 브랜치, 커밋 전). 0.3.0 정식판은 `<작업 폴더>\eu4boost_v0.3.0.zip`.
- ini: `configs/recommended.ini`(= dist/plugins/eu4boost.ini). 0.3.0 설정 + `[save] parallel_compress=1, fast_format=1`, `[modifiers] fast_add=1`, `speed5_days_per_frame=1`(꺼짐). 고정 시드·체크섬·ab_test 꺼짐.
- 사용자의 원본 GER 세이브는 손대지 않았다(백업 `<작업 폴더>\save_backup_20261015\`). 저장 목록의 `eb_GER`은 테스트용 복사본.
- 이전 dev3 DLL과 ini는 `<작업 폴더>\backup_0.3.0-dev3\`에 보관.
- **바로 다음 할 일**: §9.

---

## 2. 환경

| 항목 | 값 |
|---|---|
| PC | Ryzen 5 9600X(6코어/12스레드, Zen 5), RAM 31GB, Windows, 4K 165Hz, vsync 켬, MSAA 4 |
| 게임 폴더 | `<게임 폴더>` |
| eu4.exe | 1.37.5.0, PE TimeDateStamp **0x66FE6AB9**, SizeOfImage 0x25E0000, ImageBase 0x140000000(ASLR → 코드에서는 RVA 사용), SHA-256 9ad3efe1…771a |
| EU4dll(일본어/한국어 등 다국어 패치) | `version.dll`/`d3d9.dll` 로더가 `plugins\*.dll`을 **이름순**으로 로드(eu4boost.dll → plugin64.dll). plugin64.dll은 바이트 패턴 패치 71개를 적용하고, 매 실행 주소를 게임 폴더 `pattern_eu4_jps_2.log`에 남긴다(주소 90개) |
| 문서 폴더 | `%USERPROFILE%\Documents\Paradox Interactive\Europa Universalis IV` (logs\timer_dump_*.txt, crashes\) |
| 작업 폴더 | `<작업 폴더>` — `ghidra_export\`(functions/calls/string_refs.tsv), `ghidra_decomp\`(전체 디컴파일 64,015개 함수, 33개 파일, 151MB), Ghidra/REA 설치·진단 스크립트, 이전 배포 zip |
| Ghidra | 12.1.4 `<도구 폴더>\ghidra_12.1.4_PUBLIC`, JDK 21 `<도구 폴더>\jdk-21`. 헤드리스 디컴파일 스크립트는 `tools/ghidra/` |
| 빌드 도구 | llvm-mingw 20261006 msvcrt판 `<도구 폴더>\llvm-mingw-20261006-msvcrt-x86_64`(§4). WSL은 설치돼 있지 않음. Python 3.14 + pefile/capstone/numpy |
| 테스트용 세이브 | `save games/test.eu4`(1596년 9월 10일). **관전 상태로 저장돼 있어 콘솔 `observe`가 필요 없다**(치면 게임 상태가 바뀜, §7). 로드 → 5배속 → 1597년 10월쯤 → 종료 |
| 모드 세이브(10-15부터 주 측정 대상) | 런처 플레이세트 **짭짭죽**, `save games/eb_GER.eu4`(GER 아이언맨 1543.05.31의 복사본). 아이언맨은 불러온 이름으로 자동 저장하므로 실행마다 `save_backup_20261015\GER.eu4`에서 되돌린다. 원본 `GER.eu4`는 절대 쓰지 않는다. 절차는 ANALYSIS_LOG "모드 아이언맨 세이브" |
| 사용자 | 한국어 사용. 기술 설명이 길면 헷갈려 함 → **짧고 쉬운 한국어**로, 해야 할 행동은 번호 목록으로 |

---

## 3. 저장소 구조

```
CLAUDE.md                 Claude Code가 먼저 읽는 요약
build.sh                  빌드 (Windows Git Bash + llvm-mingw, 또는 WSL/Linux)
toolchain.sh              build.sh·run_tests.sh가 쓰는 컴파일러 찾기
build_windows.ps1         PowerShell 빌드 (build.sh와 같은 결과)
src/
  common.h                설정 구조체, 버전 문자열(EU4BOOST_VERSION), 모듈 선언
  main.c                  DllMain, ini 읽기, 로그(이전 로그는 eu4boost_prev.log로 보관), A/B 교대
  alloc.c                 UCRT 힙 → mimalloc, Windows 힙 교차 해제 가드, EU4dll 0 채우기 호환, 대형 페이지
  threads.c               IAT 유틸, 메인 스레드 CPU 고정 해제, 5배속 중간 렌더 간격, 비활성 창 감속 제거
  online.c                POPS/Nakama 콜백 펌프 간격 제한
  speed.c                 하루별 실제 시간 CSV, 일일 상태 체크섬(검증), 고정 시드(-seed=N과 같음), 5배속 한 프레임 여러 날
  parallel.c              병렬 for 통계/개선(묶음 가져가기, 작은 루프 작업 수 제한, 긴 항목 먼저(효과 없음), 워커 스핀(폐기)), 모의 자가 테스트
  savezip.c               세이브 zip 엔트리 병렬 압축, 세이브 숫자 쓰기 두 곳 빠르게, 저장 시간 통계
  modifier.c              modifier 합치기(FUN_1408ea420) O(n) 재구현, verify=1/2 진단 모드
  savepar.c               세이브의 국가 쓰기 병렬화([save] parallel_countries, 검증 모드 2/3)
  country.c               국가 업데이트 통계([country] stats), RefreshKnowledge 병렬 미리 계산 실험([country] parallel_refresh, 검증 모드 2/3, 경합 검사 4, refresh_fixup)
  profiler.c              Ctrl+Alt+P 샘플링 프로파일러 (.tsv 출력)
test/
  harness.c, fake_pops.c  스모크 테스트 (Windows는 바로, Linux는 Wine. EB_TEST 빌드는 harness의 내보낸 함수를 대상으로 후킹)
  run_tests.sh            12개 시나리오: zero_all, zero_auto, plain, online_off, no_alloc, speed, partest, savezip, savezip_off, modadd, modadd_sse2, pacing
third_party/              mimalloc v2.2.4 (MIT), MinHook (BSD-2) — 사용자 승인 받음
configs/                  ini 예시 (UTF-16LE, CRLF): recommended / ab_test_normal_mode / validation_seed1234_checksum / baseline_all_off
dist/                     배포용 README_KO.md와 권장 ini (0.4.0-dev1 반영)
validation/               기준 체크섬(baseline_seed1234_20261015.csv = 현재 기준, baseline_seed1234_0.2.4.csv = 10-14 기준으로 지금 환경에서는 재현 안 됨), 검증·A/B 로그
tools/
  re/                     eu4.exe 분석: fn.py(디컴파일 보기), disasm.py, iatrefs.py, dref.py(전역 참조), cg.py(호출 그래프), imports.py, sig.py(서명 바이트)
  analysis/               측정 분석: compare_speed.py, compare_state.py, parallel_stats.py, profile_tree.py, profile_parallel.py, profile_compare.py, timer_dump.py
  analyze_profile.py      프로파일 요약/folded stacks
  ghidra/                 DecompileAll.java, DecompTargets.java, PreOpts.java, decompile_eu4.ps1/.cmd
docs/                     이 문서, ANALYSIS_LOG.md(시간순 상세 기록), REA_SETUP_LOG.md
```

도구 경로는 `tools/re/re_paths.py`가 정한다. 환경 변수 `EU4_EXPORT`, `EU4_DECOMP`, `EU4_EXE`로 바꿀 수 있고, 기본값은 위 PC 경로다. 필요한 Python 패키지는 `pip install pefile capstone numpy`.

---

## 4. 빌드 · 테스트 · 설치

**빌드(Windows, 2026-10-15부터 기본).** 사용자 PC에서 Git Bash로 실행한다.
```sh
sh build.sh              # → build/eu4boost.dll (x64, static, stripped). 약 4초
sh test/run_tests.sh     # 12개 모두 "ALL TESTS PASSED", 끝 줄 "all 12 cases passed" (아니면 종료 코드 1). 약 40초
```
- 도구: llvm-mingw 20261006 **msvcrt판**(LLVM 23.1.3, GitHub mstorsjo/llvm-mingw, sha256 24847b37…96e1)을 `<도구 폴더>\llvm-mingw-20261006-msvcrt-x86_64`에 풀었다. PATH는 바꾸지 않았고 `toolchain.sh`가 `$LLVM_MINGW` 또는 가장 새 `<도구 폴더>\llvm-mingw-*`를 찾는다. msvcrt판을 고른 이유: 클라우드 빌드(dev3까지)도 msvcrt.dll을 쓴다.
- Windows에서는 테스트가 Wine 없이 harness.exe를 바로 실행한다.
- `build_windows.ps1`(PowerShell)도 같은 도구를 찾고, build.sh와 같은 코드(.text 동일)를 만든다.
- 같은 소스·같은 도구로 두 번 빌드하면 .rdata의 타임스탬프 몇 바이트만 다르다. dev3(클라우드 clang 18.1.3)와는 컴파일러가 달라 코드 바이트는 다르지만, 섹션 구성·가져오는 DLL·로그 문자열 72개는 같다. 로컬 빌드는 아직 게임에서 실행해 보지 않았다.

**빌드(WSL/Linux, 클라우드에서 쓰던 방법).**
```sh
sudo apt install clang lld mingw-w64 wine64    # 클라우드에서 쓴 버전: clang/lld 18.1.3, mingw-w64 11.0.1, wine 9.0
sh build.sh && sh test/run_tests.sh            # 테스트는 Wine으로 실행
```
- clang을 `--target=x86_64-w64-windows-gnu`로 쓰는 이유는 mimalloc이 네이티브 TLS를 쓰기 때문이다(mingw gcc는 emutls). `__thread`, `__builtin_return_address`, `__sync_*`를 쓰므로 MSVC로 옮기려면 수정이 필요하다.
- 같은 소스라도 빌드 경로가 바뀌면 바이너리가 몇 바이트 다를 수 있다(PE 타임스탬프 등). 동작은 같다.

**설치.**
1. 게임이 꺼져 있어야 한다(켜져 있으면 DLL이 잠겨 있음).
2. `build/eu4boost.dll`과 원하는 `configs/*.ini`를 각각 `plugins\eu4boost.dll`, `plugins\eu4boost.ini`로 복사한다.
3. 복사 후 해시를 확인한다.

- ini는 **UTF-16LE + CRLF**다(GetPrivateProfile*W). 편집할 때 인코딩을 유지할 것. Python이면 `open(p, encoding='utf-16', newline='')`.
- 실행 후 확인할 파일:
  - `plugins\eu4boost.log`: 직전 실행은 `eu4boost_prev.log`
  - `eu4boost_speed_*.csv`
  - `eu4boost_state_*.csv`: 체크섬을 켰을 때만
  - `eu4boost_profile_*.tsv`: Ctrl+Alt+P를 눌렀을 때만

---

## 5. 기능별 정리

| 기능 | ini | 방식 | 상태/효과 |
|---|---|---|---|
| mimalloc 할당기 | `[allocator] enabled=1` | UCRT `_malloc_base` 등 6개(RVA·24바이트 서명) MinHook. 이전 힙 포인터는 `__acrt_heap`(0x233d850)로 보내고, ntdll RtlFreeHeap/RtlSizeHeap/RtlReAllocateHeap 가드가 다른 DLL의 교차 해제를 처리 | 0.2 묶음 효과의 일부. 할당 자체 비용은 메인 스레드의 약 2.3%(빈 블록 목록 캐시 미스로 추정) |
| EU4dll 0 채우기 호환 | `zero_fill=-1`(자동→모드 2), `zero_fill_min_bytes=65536` | EU4dll은 `_malloc_base`의 HeapAlloc을 HEAP_ZERO_MEMORY로 바꾼다. 실제로 필요한 곳은 0x3D88→0x103D88로 키운 글꼴 버퍼뿐 → 64KB 이상만 mi_zalloc | 0.1.0은 이걸 몰라 글꼴 처리에서 크래시(0x14159f955). 지금은 안정적 |
| 대형 페이지 | `large_pages=1, reserve_mb=4096` | 옵션 설정 후 `mi_process_init()` → 예약. QueryWorkingSetEx로 실제 적용 여부를 로그에 남김 | **효과 없음**(25.87/25.20 vs 25.26) → 끔. "메모리에 페이지 잠그기" 권한은 사용자 계정에 부여돼 있음 |
| 메인 스레드 CPU 고정 해제 | `[threads] main_thread=free` | IAT SetThreadAffinityMask 후킹. 게임은 메인 스레드를 CPU (N/2+1)=6에 고정함 | 0.2 묶음 |
| 5배속 중간 렌더 간격 | `[render] speed5_midtick_frame_ms=50`(0.4부터 권장 = 원래값) | 속도별 표 RVA 0x1c33e18의 [4]를 0.05→설정값으로 | 0.2 묶음에서는 200. 중간 프레임(0x14c25a0)은 입력도 읽어서(입력 객체 vtable[1] 0x141569c70 = SDL_PollEvent) 200이면 긴 날에 스페이스바가 최대 0.2초 늦음 → 사용자 체감 문제로 50으로 되돌림(10-16) |
| 비활성 창 감속 제거 | `background_full_speed=0`(기본 꺼짐) | SDL_Delay 썽크(0x1733fa0) 후킹, 프레임 함수(0x14c25a0) 안의 Delay(100)만 건너뜀 | 옵션 |
| 온라인 폴링 제한 | `[online] poll_interval_ms=100` | IAT POPS_RunCallbacks(호출 지점 2곳, 핸들별 타이머) + Nakama tick 0x168dff0 MinHook | 메인 스레드 POPS 5.4→0.8%, Nakama 2.5→0.1%. 멀티는 0 권장 |
| 실제 속도 기록 | `[general] speed_log=1` | CDailyTickCommand::Execute(0xb79520) 후킹. 날짜는 게임상태(*0x233fe78)+0x1dd0의 CDate(시간 + 43,800,000, 1년 365일) | CSV + 로그 100일 요약. 1초 넘는 공백은 일시정지로 보고 제외 |
| 상태 체크섬 | `[debug] state_checksum=1` | 게임의 멀티 OOS 체크섬: FUN_140746c40(벡터 140항목) + FUN_140743aa0(gs, vec). 항목 0 = RNG 호출 수, 1 = RNG 시드 | 26ms/일. **검증 전용** |
| 고정 시드 | `[debug] fixed_seed=1234` | 개발자 옵션 `-seed=N`과 똑같이 0x23581c0=1, 0x23581c4=N, 0x23494b4=N, 0x23494b0=0 | 같은 세이브 2회 → 391일 × 140항목 완전 동일. 이 모드에서는 AI 유닛 루프가 **직렬**로 돌아 느림 |
| 병렬 통계 | `[parallel] stats=1` | 디스패처(0x1798a0)·대기(0x179e90)·작업 본체(0x17a200) 후킹, 호출 위치별 집계 | 부담은 무시할 수준 |
| 묶음 가져가기 | `[parallel] chunked=1` | 작업 본체와 메인 대기를 같은 구조로 재구현. 항목 수 ≥ 32×참가자면 guided 방식(남은 수/(8×참가자), 1~64)으로 묶음 확보, 메인은 기다릴 때 pause | 검증 통과. 4941항목 루프 172→48µs |
| 작은 루프 작업 수 제한 | `[parallel] small_loop_caps=1` | parallel.c `g_caps[]`: 169be36→1, db2525→2, 424462→4 (완료 콜백 없는 루프만) | 검증 통과. dev2에서 느려진 3곳(191b42, e0ff78, 77f8c1)은 dev3에서 제외 |
| 워커 스핀 | `worker_spin_us`(0=끔) | 워커 루프의 _Cnd_wait(반환 주소 0x1712dc9)만 골라 대기 전에 스핀 | **약 4% 느려짐**(SMT 짝 간섭) → 폐기, 코드만 남음 |
| 세이브 병렬 압축 | `[save] parallel_compress=1`(block_kb=2048, threads=0) | FUN_14153c790(zip 엔트리 deflate) 후킹. 2MB 블록을 게임의 zlib으로 모든 코어에서 압축, Z_SYNC_FLUSH로 이어 붙임(pigz 방식), CRC는 블록별 + crc32_combine | 압축 376 → 50ms. 내용 동일, 0.6% 커짐 |
| 세이브 숫자 쓰기 | `[save] fast_format=1` | 호출 지점 2곳(0x421409 "%d", 0x246c31 "%d.%03d")을 rel32로 빠른 함수에 연결 | 저장 520 → 405ms. 바이트 동일 |
| 저장 통계 | `[save] stats=1` | FUN_1405ce5f0 시간 + 압축 시간 로그 | 측정용 |
| modifier 합치기 | `[modifiers] fast_add=1`, `fast_search=1`(기본) | FUN_1408ea420 후킹. sparse dst에 4쌍 이상 더할 때 스레드별 id 표로 O(n). 나머지(1~3쌍, dense dst)는 원래 알고리즘을 그대로 하되 id 찾기를 AVX2(없으면 SSE2)로 8쌍씩. dst == src만 원래 함수 | **−7.6%**(모드 세이브). verify=1(원래 함수 결과와 매번 비교), verify=2(빠른 경로 + 원래만큼 시간 끌기), verify=3(SIMD 검색 경로 + 단순 검색 재실행 비교)는 진단용. fast_search=2는 SSE2 강제(테스트용) |
| 긴 항목 먼저 | `[parallel] longest_first=1`(기본 0) | AI 유닛 루프(0x77d9b5)를 지난 호출의 항목 시간 순으로 | 효과 없음(가장 긴 항목 8.5ms가 벽시간을 정함) |
| 한 프레임 여러 날 | `[render] speed5_days_per_frame=1`(기본, 2~3이면 켬), `speed5_frame_budget_ms=100` | 세션 업데이트 래퍼(0x20f810) 후킹. 하루가 진행됐으면 게임 타이머(0x757ad0)를 0초로 불러 다음 날을 넣고 세션 업데이트를 다시. 속도 5·싱글·1일 아님·예산 안에서만 | 2일 −3.4%, 3일 −5.2%. 화면 갱신 초당 약 18 → 10 / 7번 |
| 세이브 나라 병렬 쓰기 | `[save] parallel_countries=1`(코드 기본 0, 권장 ini 1), 2 = + 검증, 3 = 한 스레드 + 검증 | 국가 루프(0x140751760)의 FUN_141707a20 호출(반환 0x14075179a) 첫 번에 모든 나라를 작업 스레드 11개 + 메인에서 각자 게임의 메모리 스트림(생성자 0x171ebc0, vtable 0x1dc0358)과 writer 복사본(0x20바이트)으로 씀 → 호출마다 그 나라 바이트를 진짜 스트림에 vt+0x50으로 붙임. 공유 정적 serializer 2개(FUN_14015bf70·FUN_140451740 → 0x142458bf0, FUN_1402c59b0 → 0x142458e80)는 작업 스레드에서 스레드별 복사본. 지연 생성 객체 6개 중 하나라도 없으면 그 저장은 원래 방식 | 저장 410 → 245ms. 국가 1,668개 44MB를 47ms(한 스레드 261ms) |
| 국가 업데이트 통계 | `[country] stats=1` | 국가 루프·국가별 업데이트·RefreshKnowledge·CalculateModifier·UpdateCasusBellis 후킹(호출 수, 시간 분포, 호출 위치) | 측정용 |
| RefreshKnowledge 병렬 미리 계산 | `[country] parallel_refresh=1`(**실험, 기본 0**), 2 = + 검증, 3 = 한 스레드 + 검증, 4 = 경합 검사. `refresh_fixup=1`(기본) | 국가 루프 시작에 변경 표시(+0x2882) 나라의 RefreshKnowledge를 작업 스레드 11개 + 메인에서 실행, 표시를 2로. 묶음 호출(반환 0x2f087b)에서 2면 건너뛰고 FUN_1402f1120(국가, 1)만 다시 실행(+0xe1c~+0xe34 캐시, 4µs). 다른 나라의 UpdateOpinionCache(0x38b340)는 기록 후 메인에서 순서대로 실행. 그룹(+0x1948) 지도자도 같은 단계에 있는 나라는 제외(지도자의 +0x1bf0 목록을 읽기 때문, 약 4%). 플레이어·관전 나라 제외, 멀티 제외 | 국가 루프 −1.0ms/일. 원래 시점 재계산과 비교 7,552번 중 8번만 다름(+0xdbc, +0x2030/34: 그날 바뀐 프로빈스 값), 경합 검사 7,678번 중 0번 |
| A/B 교대 | `[parallel] ab_test=1/2/3/4` | 실행마다 짝수=A, 홀수=B. 1: 병렬 실험 끔, 2: longest_first·fast_add 끔, 3: 한 프레임 1일, 4: parallel_refresh 끔, 5: fast_search 끔. 카운터는 `plugins\eu4boost_ab.txt` | 측정용 |
| 프로파일러 | `[profiler] enabled=1` | Ctrl+Alt+P로 시작/정지. SuspendThread + RtlVirtualUnwind(.pdata 직접 조회) | 측정 중에는 누르지 말 것(부담 있음) |

---

## 6. 주요 주소 (eu4.exe 1.37.5.0, RVA = VA − 0x140000000)

**플러그인이 쓰는 주소.** 모두 서명을 확인한다. 서명 바이트는 `tools/re/sig.py`로 다시 뽑을 수 있다.

| RVA | 무엇 |
|---|---|
| 0x1a6837c / 0x1a6989c / 0x1a6833c / 0x1a6dd98 / 0x1a706f4 / 0x1a67bf8 | UCRT `_malloc_base` / `_calloc_base` / `_free_base` / `_realloc_base` / `_msize_base` / `_recalloc_base` |
| 0x233d850 | `__acrt_heap` |
| 0x1a683ae | `_malloc_base` 안 HeapAlloc (EU4dll "Font buffer heap zero clear" 패치 지점) |
| 0x14c25a0 | 프레임 함수(CApplication 루프 본체), 0x14c2d00 CApplication::Run(메인 스레드 고정) |
| 0x1733fa0 | SDL_Delay dynapi 썽크 |
| 0x1c33e18 | 속도별 틱 중간 렌더 간격 표 float[5] = {0.02×4, 0.05} |
| 0x14ada90, 0x14ada9d / 0x14c5adb | CPopsWrapper::Update, POPS_RunCallbacks 호출 지점 2곳 |
| 0x168dff0 | Nakama 클라이언트 tick |
| 0xb79520 | CDailyTickCommand::Execute (timercommands.cpp) → 0x758430 일일 틱 |
| 0x233fe78 | 게임상태 포인터 (CDate 현재 날짜 = gs+0x1dd0) |
| 0x746c40 / 0x743aa0 | OOS 체크섬 벡터 생성 / 계산 |
| 0x23581c0 / 0x23581c4 / 0x23494b4 / 0x23494b0 | 고정 시드 플래그 / 고정 시드 값 / RNG 시드 / RNG 호출 카운트 |
| 0x1798a0 | 병렬 for 디스패처 (self, ?, start, count, item_fn*, done_fn*) |
| 0x17a070 | 공유 상태 생성 (out, mode, int* ntasks, start, count). *ntasks<1이면 하드웨어 스레드−1 |
| 0x17a200 / 0x179e90 | 작업 본체 / 메인 참여+대기 |
| 0x1712d70, 0x1712dc4 | 풀 워커 루프, 그 안의 _Cnd_wait 호출 |
| 0x242b8e0, 0x242b950 | 작업 풀 2개 {mtx*, cv*, deque…, size +0x30} |
| 0x1a139cc / 0x1a139f0 / 0x1a13b48 | STL _Mtx_lock / _Mtx_unlock / _Cnd_wait 래퍼 |
| 0x1581820 | 바이트 스핀락 획득 (디스패처 객체+8) |
| 0x1a0c164 | std::bad_function_call 던지기 |
| 0x5ce5f0 | eu4savegamehelper: 세이브 하나 쓰기(zip meta / gamestate / ai) |
| 0x153c790 | zip 엔트리 하나 deflate (zip, in_stream, buf, buf_size, uint* crc, level 3) → 압축 크기 |
| 0x181fc70 / 0x181f210 / 0x181fb90 | 게임 안의 zlib deflateInit2_ / deflate / deflateEnd. CRC 표 0x1d91cd0 |
| 0x421409 / 0x246c31 | 세이브 숫자 쓰기 호출 지점(FUN_14015c230 "%d" / FUN_141657e40 "%d.%03d") |
| 0x8ea420 | modifier 합치기(dst += src). 호출: operator new 썽크 0x1a33310(+0x101), push_back 0x8f0e0(+0x1dc) |
| 0x20f810 | 세션 업데이트 래퍼(app session) → CSession::Update 0x14ca360 |
| 0x757ad0 | 게임 타이머(gs, float 초): 하루 길이(표 0x1c33e30)에 도달하면 CDailyTickCommand 하나 넣음 |
| gs+0x1e08 / gs+0x1e00 | 게임 속도 인덱스(4 = 5배속) / 세션 포인터(세션[0x6c] = 멀티 날짜 동기화) |
| 0x75b0d0 / 0x2ee280 | 국가 직렬 루프(gs+0x1d08 국가 포인터 목록, 수 +0x1d14) / 국가 하루 업데이트 |
| 0x93fe30 / 0x327dc0 / 0x4426f0 | RefreshKnowledge(국가, where) / CalculateModifier(국가, where, char) / UpdateCasusBellis 래퍼 |
| 0x2f0876 / 0x2f0888 / 0x2f08a9 / 0x2f08b1 | 변경 표시 묶음의 호출 지점(RefreshKnowledge / CalculateModifier / UpdateCasusBellis / 0x3d00c0). 묶음 조건 country+0x2882 != 0 |
| 0x38b340 | UpdateOpinionCache(국가, 관계 키, char): RefreshKnowledge가 다른 나라에 대해 부름, 0x38b1f0을 거쳐 병렬 for까지 감 |
| 0x751760 / 0x75179a | 세이브 국가 루프(gamestate+0x1ce8 목록, 개수 gamestate vt+0x40) / 그 안의 FUN_141707a20 호출 반환 주소 |
| 0x1707a20 | FUN_141707a20(writer, std::string* 키, 객체): 키 = { 객체 } (객체 vt+8로 씀) |
| 0x1708870 / 0x1709280 / 0x1708750 / 0x17087e0 | writer 토큰 쓰기 / 토큰 / 블록 열기 / 닫기. writer 0x20바이트 {vtable 0x1dbf7d8, 깊이 +8, 스트림 +0x10, 이진 +0x18, 스트림 소유 +0x19} |
| 0x171f290 | 메모리 스트림 쓰기(vt+0x50). 위치 +0x4c, 고정 버퍼 +0x40/크기 +0x50/사용 +0x48, 그 뒤 std::vector<char>* +0x38. vt+0x48은 1바이트 쓰기(yes/no) |
| 0x2f1120 | FUN_1402f1120(국가, char): RefreshKnowledge 첫 단계, +0xe1c~+0xe34 캐시 계산(refresh_fixup이 다시 부름) |
| 0xb4a470 | FUN_140b4a470(그룹, 태그, 태그): 그룹(+0x1948) 지도자 나라의 +0x1bf0 목록 검색 — RefreshKnowledge의 다른 나라 읽기(경합 원인) |
| 0x14c25a0 / 0x1569c70 | 프레임 함수(중간 렌더도 이것) / 입력 객체 vtable[1] SDL_PollEvent 루프 |
| 국가 필드 | +0x20 태그, +0x1338/+0x1344 소유 프로빈스(int 목록, 수), +0x1510/+0x1518 관계 배열(태그당 0x48), +0x18f0 종주국 태그, +0x2882 / +0x2885 변경 표시, 객체 크기 0x9b50 이상(mi_usable_size는 국가에 못 씀) |
| gs+0x1e60 / gs+0x1e68 | 플레이어 태그 / 관전 중인 태그(gs+0x1e66 == 7이고 +0x1e6f일 때) |

**병렬 상태(ctx) 배치.**
- +0x38: 완료 콜백 std::function impl
- +0x40: 시작
- +0x44: 다음 항목(원자적)
- +0x48: *완료 수(int*, 처음 값 = 시작)
- +0x50: 소진 플래그
- +0x58: *완료 표시
- +0x60: *취소
- +0xa0: 항목 함수 impl(vtable[2](self, int*))
- +0xa8: 끝
- +0xb0: *참가 종료 수
- +0xb8: 작업 수

완료 콜백은 (작업 수−1)번째 참가자가 끝날 때 불린다. 그래서 콜백이 있는 루프는 작업 수를 바꾸면 안 된다.

**분석에 쓴 주요 함수(VA).**

| 영역 | 함수 |
|---|---|
| 일일 틱 | 0x140758430 → 0x14075bbe0 |
| 국가 | 국가 직렬 0x14075b0d0 → 국가 일일 업데이트 0x1402ee280 |
| AI·월간 | AI 유닛 0x14075b4b0/0x14077d8a0(항목 0x1400b0fa0), 월간 0x14075d690/0x1402f36c0 |
| 국가 하위 작업 | RefreshKnowledge 0x14093fe30, CalculateModifier 0x140327dc0, UpdateCasusBellis 0x1404426f0, CUnit::DailyUpdate 0x140d3e110 |
| 국가별 작은 병렬 루프 | 0x14038b1f0 → 0x140424380 |
| 시드 | 로드/새 게임 때 시계로 다시 정하는 함수 0x140ad0880, 세이브 쓰기 0x14074ff30 / 읽기 0x140751cd0(토큰 0x2dc6 seed, 0x2dc7 count), 멀티 시드 동기화 명령 실행 0x140847960 |
| 세이브 쓰기 | 게임상태 0x14074ff30 → 객체 쓰기 0x140d2f410 → 국가 0x1402dc6b0(저장 시간의 약 절반), 프로빈스 0x1409d6e60. 스트림 쓰기 0x14171f290, 체크섬 0x14171f3f0 |
| AI 유닛 루프 내부 | 항목 0x1400b0fa0 → 0x14014b370 → 국가 AI 0x1400e4890(트리거 평가가 대부분) |

---

## 7. 측정 방법

1. **실제 속도(기본).** speed_log CSV를 쓴다. 두 실행을 `python tools/analysis/compare_speed.py A=a.csv B=b.csv`로 비교한다.
   - 같은 세이브와 같은 구간이어야 한다.
   - 콘솔 `timer_dump`는 하루 계산 시간만 재고 날짜 사이(렌더, 온라인 등)는 빠지므로 보조로만 쓴다.
2. **A/B 교대.** `ab_test=1` 상태에서 두 번 연속 실행하면 `eu4boost_prev.log`(A)와 `eu4boost.log`(B)가 남는다. `python tools/analysis/parallel_stats.py eu4boost_prev.log eu4boost.log`로 루프별 비교한다.
3. **결과 불변 검증(로직 변경 시 필수).**
   - 설정: `configs/validation_seed1234_checksum.ini`에 실험 옵션을 더한다.
   - 실행: test.eu4를 불러와 **콘솔을 쓰지 않고** 5배속으로 400일쯤. 콘솔 명령(stats 외 아무거나)을 치면 체크섬 c2에 '콘솔 사용' 표시(0x1b46b1)가 붙고, 이미 관전 중일 때 `observe`를 치면 둘째 날부터 게임 상태도 달라진다.
   - 비교: `python tools/analysis/compare_state.py validation/baseline_seed1234_20261015.csv eu4boost_state_*.csv` → 모든 날짜가 IDENTICAL이어야 한다.
   - 기준이 의심되면 같은 DLL로 실험을 끈 실행(chunked=0, small_loop_caps=0)을 그 자리에서 하나 더 돌려 그것과 비교한다. 10-14 기준(0.2.4)은 10-15 환경에서 둘째 날부터 범주 5·57·89·90·94가 조금씩 달라 재현되지 않는다(원인 미확인, 난수 사용 수 c0는 같음).
   - 한계: 이 모드에서는 AI 유닛 루프가 직렬이라 검증 범위에서 빠진다(Paradox도 이 루프를 비결정적으로 보는 듯).
   - **한계 2(10-15 발견): 이 게임은 실행 속도에 따라 결과가 조금 달라진다.** modifier 합치기 개선(fast_add)을 켜면 1544.03.02부터 체크섬이 갈라졌지만, 같은 계산에 원래만큼 시간을 끌면(verify=2) 299일 동일했다. 그래서 속도를 꽤 바꾸는 개선은 고정 시드 비교가 갈라질 수 있고, 그때는 (a) 호출마다 원래 함수 결과와 비교하는 진단 모드(예: `[modifiers] verify=1`)와 (b) 원래 시간을 되돌린 실행으로 검증한다. 세이브 개선(압축·숫자 쓰기)과 0.3.0 병렬 개선은 고정 시드에서 그대로 동일했다.
   - 세이브를 바꾸는 개선은 저장된 세이브의 압축을 풀어 바이트 비교한다(gamestate/meta/ai).
4. **모드 아이언맨 세이브(eb_GER) 실행.** ANALYSIS_LOG "모드 아이언맨 세이브"의 절차. 런처 → 혼자서 하기 → 세이브 탭 → eb_GER → 시작 → 로그 "save: written" → 5배속 → 1545.06쯤 → `Stop-Process -Name eu4 -Force` → eb_GER를 백업에서 되돌림. 754일 한 번에 약 41초, 같은 설정 두 번의 차이 약 1%.
5. **프로파일.**
   - 5배속 중 Ctrl+Alt+P를 눌러 시작하고, 1~2분 뒤 다시 눌러 끝낸다.
   - 결과 파일로 `profile_tree.py`(호출 트리), `profile_parallel.py`(병렬 효율), `profile_compare.py`를 돌린다.
   - eu4boost.dll 내부 기호가 필요하면 `-s` 없이 같은 오브젝트로 링크한 쌍둥이 DLL을 쓴다(.text 배치 동일).

---

## 8. 측정 결과 요약 (시간순)

| 시각 | 실험 | 결과 |
|---|---|---|
| 10-14 20:3x | timer_dump A/B(0.1.1, 1445 세이브) | FULL_TICK −10% |
| 21:43/21:46 | 프로파일 1차 | 일일 틱 74%, 국가 일일 업데이트 40%, 병렬 대기 11%, POPS+Nakama 약 5%, RefreshKnowledge 9.7% |
| 22:12 | 0.2 켬 프로파일 | POPS/Nakama 거의 사라짐, 일일 틱 비중 82%. 병렬 루프별 효율 표 → ANALYSIS_LOG |
| 22:28/22:33 | 실제 속도 A/B(0.2.1) | 19.15 → 25.26일/초(**−24.2%**, 408일 중 370일 더 빠름) |
| 23:45/23:47 | 대형 페이지 2회 | 25.87, 25.20 → 효과 없음, 같은 설정 간 차이 약 2.6% |
| 23:53/23:55 | 체크섬 2회(시드 고정 없음) | 첫날부터 다름 → 로드할 때 시계로 시드를 정하는 것을 발견 |
| 10-15 00:06/00:08 | 체크섬 2회(fixed_seed=1234) | **391일 완전 동일** → 기준 확보 |
| 00:14/00:16 | 일반 모드 A(stats) / B(+워커 스핀) | A 24.67, B 23.65일/초(**스핀 −4%**). 루프별 통계 확보 |
| 00:31/00:34 | 고정 시드 A / B(dev2: 묶음 + 제한 6곳) | **두 실행 모두 기준과 동일**. 루프 시간 172→48, 56→34, 21→8µs 등. 3곳은 느려짐 → dev3에서 제외 |
| 10-15 12:00/12:04 | 일반 모드 A/B(dev3, Claude가 직접 실행) | 같은 536일 23.31 → 23.98일/초(**−2.8%**, −1.19 ± 0.86ms/일). 큰 루프 −32~−74%, 느려진 루프 없음 |
| 12:09~12:32 | 0.3.0 고정 시드 검증 + 권장 ini 실행 확인 | 10-14 기준은 환경 차이로 재현 안 됨 → 그 자리에서 실험 끈 기준을 다시 만듦. **0.3.0(켬)·dev3(켬)이 새 기준과 441/407일 완전 동일**. 콘솔 `observe`는 결과를 바꿈(§7). 권장 ini로 319일 정상 |
| 10-15 오후 | 모드 아이언맨 세이브(eb_GER) 0.3.0 고정 시드 | 끔 vs 0.3.0 **459일 동일**. 분기 자동 저장이 메인의 약 15% |
| 오후 | 세이브 병렬 압축 / 숫자 쓰기 | 저장 848 → 520 → **405ms**. 세이브 내용 바이트 동일, 고정 시드 303/300일 동일 |
| 오후 | modifier 합치기(fast_add) | 원래 함수와 32,144번 비교 불일치 0. 고정 시드는 1544.03.02부터 갈라짐 → 시간을 되돌리면(verify=2) 299일 동일 = 속도 의존. A/B 754일 17.33 → 18.75일/초(**−7.6%**) |
| 오후 | longest_first(AI 유닛 루프) | 효과 없음 |
| 20:48~21:07 | 한 프레임 여러 날(3일 / 2일 / 1일) | 같은 747일 19.22 / 18.86 / 18.22일/초 → 3일 −5.2%, 2일 −3.4%. 화면 갱신 초당 약 7 / 10 / 18번 |
| 20:57 | 프로파일(검증된 기능 켬) | 자동 저장 9.4%, AI 루프 대기 7.5%, RefreshKnowledge 3.3%, mi_malloc 1.9%. 나머지는 1% 안팎으로 평평함 |
| 21:13~21:25 | 할당기 prefetch A/B 2쌍 | 평균 −0.3%(잡음) → 되돌림 |
| 21:57~22:01 | 국가 업데이트 통계(500일) | 국가 루프 13.1ms/일(하루의 25%). 변경 표시 묶음 3.2ms/일, 그중 난수 없이 옮길 수 있는 것은 RefreshKnowledge·CalculateModifier |
| 22:27~22:55 | RefreshKnowledge 병렬 검증(모드 2·3) | 첫 실행 검증 코드 memcpy 크래시(국가 크기) → 수정. 이후 18년 + 2년 + 2년 크래시 없음. 불일치 4~7%, 거의 +0xe34 한 필드(한 스레드에서도 같음) |
| 22:57~23:11 | RefreshKnowledge 병렬 A/B(A·B·B·A) | 41.39/41.61초 → 40.85/40.71초, **−1.7%** |

---

## 9. 다음 단계

0. **국가 업데이트 병렬화(10-15 밤, 사용자 요청).** RefreshKnowledge 병렬 미리 계산을 실험 기능으로 만듦(−1.7%, 기본 꺼짐). 10-16: +0x1fdc는 실제 경합이었음(그룹 지도자의 +0x1bf0 목록을 동시에 다시 만듦, 해제된 메모리를 읽을 수도 있었음) → 그런 나라 제외로 해결. +0xe34는 FUN_1402f1120 재실행으로 보정. 사용자는 본 게임에 켜고 씀(본인 결정). 더 넓히려면: CalculateModifier는 다른 나라의 modifier 컨테이너를 읽어 동시 재구성과 겹치면 해제된 메모리를 읽을 위험, 나머지 국가 업데이트는 난수를 써서 순서를 바꿀 수 없음.
0-1. **0.4.0-dev1 마무리(사용자 결정 대기).** `save-compress` 브랜치에 커밋돼 있다(원격 저장소 없음). 결정할 것: 릴리스(zip) 여부, 코드 기본값(지금은 `[save]`·`fast_add`가 코드 기본 0이고 권장 ini에서 켬), `speed5_days_per_frame`을 권장 ini에서 켤지(부드러움과 바꿈).
1. ~~0.3.0 정식판~~ 끝(10-15, ANALYSIS_LOG). 10-14 기준이 재현되지 않은 원인은 실행 속도 의존(§7 한계 2)으로 보인다.
2. **병렬 디스패치 비용(호출당 20~30µs).**
   - 원인: 작업마다 operator new 3번, future 상태(mutex·condvar), 큐 잠금과 notify_one.
   - 하루 수십 번 불리는 루프에서 누적된다.
   - 후보: 디스패처를 재구현해 작업 객체를 재사용하거나 깨우는 스레드 수를 일감에 맞추기. 완료 콜백·future 의미를 그대로 지켜야 한다.
3. **직렬 로직 분산(사용자가 원하는 큰 목표).** 아직 미착수. 조사 결과와 계획:
   - **구성.** 국가 일일 업데이트(메인 시간 39%)는 40개 넘는 하위 작업의 묶음이다. 가장 큰 것: RefreshKnowledge 7.2%, 부대 약 6%, 신분 4.2%, CalculateModifier 3.8%, 국가별 병렬 루프 3%, UpdateCasusBellis 2.2%.
   - **dirty 묶음.** country+0x2882가 켜져 있으면 RefreshKnowledge → CalculateModifier(country.cpp:9210) → UpdateCasusBellis → 0x1403d00c0을 실행한다. 국가끼리 독립적일 가능성이 있어, 하루 시작에 병렬로 미리 계산하는 방식이 후보다.
   - **위험 요소.**
     - DAT_141fd10c4(난수 금지 구역 플래그)를 전역으로 토글한다.
     - 지연 초기화 싱글턴이 있다.
     - 다른 국가·프로빈스에 쓰는지 아직 모른다.
     - dirty 표시가 빠짐없이 되는지(완전성)도 모른다.
   - **진행 순서.** 쓰기 범위 분석 → 시험 구현 → 고정 시드 체크섬으로 검증. 다르면 갈라진 항목과 날짜로 원인을 추적한다.
   - **엔진의 단서.** 병렬 구간 진입 전에 DAT_141fd10c4=1로 "난수 금지 구역"을 표시하고, 위반하면 "Calling random inside forbidden area!" 로그를 남긴다 → 병렬로 돌리는 코드가 난수를 쓰는지 확인하는 데 쓸 수 있다.
4. 기타 후보: 장면 갱신 0x1414c5810. **매월 1일 처리(10-16 분석, ANALYSIS_LOG "매월 1일 처리")**: 모드 세이브에서 1일 계산 약 320ms(보통 날 37ms) = 일일 틱의 약 17%. 구성은 국가 월간 MonthlyUpdateSerial(국가마다, 하위 작업 수백 개가 각 1% 이하) 57%, 프로빈스 월간 1409f35c0(프로빈스마다, 난수를 먼저 쓰고 같은 프로빈스 값을 바꾼 뒤 CalculateModifier) 19% → 둘 다 순서대로 난수를 써서 병렬화·미리 계산 불가. 큰 덩어리가 없어 modifier SIMD 검색만 적용.
5. **10-15 밤 프로파일 기준 남은 후보(모두 작거나 위험).** 자세한 것은 ANALYSIS_LOG "새 프로파일 (20:57)".
   - modifier 합치기의 작은 덧셈(src 1~3쌍, 1,680만 번): 큰 dst 선형 검색을 SIMD로. 예상 0.5% 안팎.
   - ~~국가 세이브 쓰기 병렬화~~: 10-17 완료(`[save] parallel_countries`). 남은 저장 시간 245ms 중 큰 것: 프로빈스 쓰기(1409d6e60, 약 60ms), MD5 체크섬(14171f3f0, 약 50ms, 직렬), 압축 50ms.
   - UpdateCasusBellisGainLostCache(4%), RefreshKnowledge(3.3%): 병렬화하려면 쓰기 범위 분석 필요(아래 3번과 같은 문제).
6. 멀티플레이는 검증하지 않았다. 권장: `[allocator] enabled=0`, `[online] poll_interval_ms=0`, 병렬 실험 끔, `[modifiers] fast_add=0`. 한 프레임 여러 날은 멀티에서 스스로 꺼진다.

---

## 10. 실패와 교훈

- **0.1.0 크래시.** EU4dll이 malloc의 0 채우기에 의존하는 것을 몰랐다 → 후킹 전에 `pattern_eu4_jps_2.log`의 패치 주소(90개)와 겹치는지 꼭 확인할 것.
- **할당 비용 오해.** "0 채우기 비용"으로 봤던 2%는 실제로는 할당 자체의 비용이었다.
- **timer_dump의 한계.** 날짜 사이 시간을 재지 않아 온라인 폴링 개선이 안 보였다 → speed_log를 만들었다.
- **0.2.1 날짜 버그.** 날짜를 명령+0x50에서 읽었는데, 싱글플레이에서는 이 값이 비어 있어(1.1.1) 날짜가 모두 1.01.01로 찍혔다 → 게임상태+0x1dd0에서 읽도록 수정.
- **mimalloc 초기화 순서.** mimalloc은 allow_large_os_pages를 첫 할당 때 한 번만 읽는다 → 옵션을 켠 뒤 `mi_process_init()`을 직접 부른다.
- **SMT 간섭.** 워커 스핀은 같은 코어의 메인 스레드를 늦췄다. 작업 수를 줄였더니 오히려 느려진 루프도 있었다. 반드시 루프별로 측정할 것.
- **로그 덮어쓰기.** 다음 실행이 로그를 덮어써 A 기록을 잃었다 → `eu4boost_prev.log`로 보관한다.
- **고정 시드의 한계(10-15).** 속도를 바꾸는 개선은 로직이 같아도 체크섬이 갈라질 수 있다 → 호출별 비교(verify)와 시간 복원 실행으로 검증한다.
- **한 프레임 여러 날 v1.** 세션 업데이트만 한 번 더 부르면 다음 날이 아직 큐에 없어 아무 일도 없다 → 게임 타이머를 0초로 불러 하루를 넣어야 한다.
- **측정 스크립트.** 새 실행의 CSV가 생기기 전에 "가장 새 CSV"를 고르면 이전 실행 파일을 읽는다 → 불러오기가 끝난 뒤 CSV를 고르고 첫 날짜를 확인한다.
- **할당기 prefetch.** 프로파일에서 mi_malloc의 캐시 미스가 커 보여도, 다음 블록 prefetch로는 실제 속도가 안 바뀌었다(비용이 호출자로 옮겨갈 뿐).
- **클라우드 세션 쪽 문제.**
  - REA로 eu4.exe를 직접 열면 60초 제한에 걸린다.
  - 브리지 staging에서 같은 경로를 다시 쓰면 이전 내용이 commit될 수 있다(매번 새 폴더를 쓰고 sha256으로 확인).
  - 자동 승인 판정이 시간 초과로 막힌 적이 있다.

---

## 11. 클라우드 세션 쪽 자료 (참고)

- **claude.ai 프로젝트 "유로파4".**
  - `claude/EU4 성능 분석 기록.md`: 이 저장소의 docs/ANALYSIS_LOG.md와 같은 내용.
  - `claude/REA 설치 기록.md`
- **PC 게임 폴더 `plugins\`.** 모든 실행의 speed/state CSV, profile tsv, 로그가 남아 있다. 파일 이름의 시각이 §8 표와 대응한다.
- **이전 배포 zip.** `<작업 폴더>\eu4boost_v0.1.zip`, `v0.1.1`, `v0.2.0`, `v0.2.1`.
