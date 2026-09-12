# Phase 1.8 실험 인계 문서 (2026-09-12)

다른 환경에서 이어서 작업하기 위한 요약. 상세 설계/과정은 [plan.md](plan.md)에 전부 있고, 이 문서는 그중 **실측 결과와 지금 상태**만 발췌·정리한 것.

## 기준 커밋 / 환경

- 베이스 커밋: `780da055fab1dd1adee2b8e2f188ddb0c7326642`("Modern C++ scheduler policies (FCFS/RR, CFS in progress) + mmap C++ port") — 이 문서가 다루는 변경분은 전부 이 위에 **아직 미커밋** 상태.
- 컴파일러: `riscv64-linux-gnu-g++-14`/`gcc-14` (Ubuntu 14.2.0-4ubuntu2~24.04.1) 14.2.0, `-std=c++23`.
- QEMU: 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.16).
- `Makefile`: `GCC_VER=-14` 고정.

## 진행 상황

| Phase | 상태 | 요약 |
|---|---|---|
| 0 | ✅ | `r_cycle`/`r_instret` 추가. **`-icount shift=0` 없이는 QEMU `instret`이 부정확**하다는 걸 실측으로 발견(20회 반복해도 기댓값의 ~1.8배에서 플래토) — 이후 모든 정밀 측정의 전제조건. |
| 0.5 | ✅ | `-fanalyzer` 채택, C++20 모듈은 이 툴체인 미지원 확인 후 기각. |
| 1 | 🟡 | FCFS/RR을 concept+템플릿 정적 디스패치로 구현. mmap C++ 포팅 완료. |
| 1.8 | 🟡 진행 중 | **현재 초점.** 우선순위 기반 선점 정책 하나만 RR과 비교. 아래 상세. |
| 2 (CFS) | ⏸️ 보류 | 설계만 있고 구현 안 함 — Phase 1.8이 범위를 좁히면서 후순위로 밀림. |

## 핵심 실험 1 — 정적 vs 함수포인터 디스패치 (Phase 1)

목적이 "C++로 대기시간을 줄였다"가 아니라 **"정책을 정적으로 구성하는 C++ 구조의 실행 비용을 이해하는 것"**임을 분명히 할 것. 원시 로그: [docs/bench/](docs/bench/).

`pick_next()` 200회 호출, `-icount shift=0`/CPUS=1로 측정 (instret/call):

| 조건 | static | fnptr | 차이 |
|---|---|---|---|
| trivial O(1) 본체 | 2 | 14 | +12 (호출경로 순수비용) |
| RR 본체, n=상수(64) | 707 | 533 | **−174 (역전!)** |
| RR 본체, n을 volatile로 불투명화 | 517 | 533 | +15 |

**해석:** 처음 관측된 "정적이 174/call 느림"은 디스패치 메커니즘 차이가 아니라, 인라인이 컴파일러에게 `n=64`라는 상수를 노출시켜 GCC가 `%64`를 소프트웨어 강도감소(5개 명령)로 바꾼 탓 — n을 불투명화하면 정적이 다시 이긴다(+15/call, trivial 케이스의 +12와 거의 일치). **성과 표현: "간접 호출 비용과 컴파일러 최적화 효과를 분리해 분석했다."**

## 핵심 실험 2 — Phase 1.8: 우선순위 기반 선점

### 설계

- `SchedulerPolicy` concept에 `should_preempt(running) -> bool` 추가(기존 `pick_next`와 별개 축).
- `struct proc`에 POD 필드 `int priority;`(0=Normal, 1=LatencySensitive) 추가 — **명시적 syscall(`setpriority`)로만 설정, 실행시간으로 추론 안 함.**
- `PriorityPreempt<UseCounter>`: `pick_next()`는 RUNNABLE 중 LatencySensitive를 최우선 선택(동순위는 RR), `should_preempt()`는 Normal이면 RR과 동일하게 매 tick 무조건 true, LatencySensitive면 false(끊지 않고 끝까지 돌림).
- `UseCounter`(템플릿 bool): `pick_next()`의 1차(LatencySensitive) 스캔을 O(1) 카운터(`g_ls_runnable_count`)로 건너뛸지, 항상 O(n) 스캔할지 — **판단비용 비교 축은 여기.**
- 커널 훅: [kernel/trap.c:94-97](kernel/trap.c:94)의 `usertrap()`만 수정(무조건 `yield()` → `policy_should_preempt(p)` 게이트). `kerneltrap()`은 의도적으로 안 건드림 — **정책이 전체 실행 경로에 일관 적용된 건 아님.**

### 발견·수정한 실제 버그 (커널)

1. **인터럽트 행(hang) 버그:** `run_dispatch_bench()`에서 `intr_off()`/`intr_on()`을 호출한 게 원인 — `start.cpp`의 `timerinit()`이 M-mode에서 이미 타이머 인터럽트를 예약해뒀는데 `stvec`은 `trapinithart()`(더 나중) 전엔 미설정이라, `intr_on()`이 예약된 인터럽트를 조기에 풀어버려 CPU가 정의되지 않은 주소로 트랩되어 무한 정지(패닉 출력도 없음). 수정: `intr_off()`/`intr_on()` 호출 제거(이 시점엔 애초에 이미 비활성이라 필요 없음).
2. **`should_preempt()` 설계 버그(1차):** 최초 구현은 Normal에 대해 `g_ls_runnable_count > 0`을 봤는데, 아무도 `priority=1`을 쓰기 전엔 이게 항상 0이라 **Normal이 영원히 양보를 안 함** — `usertests -q`의 `test preempt`가 여기서 멈춰 잡음. 수정: Normal은 RR과 완전히 동일(무조건 true)하게, 우선순위 메커니즘의 효과는 전부 `pick_next()`의 재정렬에서만 나오도록 정정.

### 측정 인프라 버그 (사용자 프로그램, [user/latencytest.c](user/latencytest.c))

외부 검토로 발견, 전부 수정 완료:

1. **`wait(0)` 오회수:** [kernel/proc.c:449](kernel/proc.c:449)의 `kwait()`는 `proc[]`에서 **처음 찾은 ZOMBIE 자식**을 회수(호출자가 기다리는 pid와 무관). 배경 작업(A)이 종료하는 순간과 B를 기다리는 `wait(0)` 호출이 겹치면 A를 대신 회수 — 응답시간 표본이 오염됨. 수정: `wait_for(target)` 헬퍼로 기대한 pid가 나올 때까지 재시도.
2. **P99 표본 부족:** `NSHORT=20`에선 `(20*99)/100=19` → 사실상 최댓값. `NSHORT=100`으로 올림.
3. **A 강제종료로 처리량 미측정:** `kill()` 제거, A가 자연 완료하도록 바꿔 완료 tick을 직접 비교.

**미수정, 알려진 한계:**
- `should_preempt()`에 실행시간 상한이 없음 — "지연 민감"이 "짧다"를 보장 안 함. LatencySensitive가 실제로 길게 돌면 다른 LatencySensitive까지 무기한 굶길 수 있음(설계상 진짜 취약점, budget 축 도입 시 해결 예정).
- B의 최초 디스패치(~4tick)는 두 정책 모두 동일 — 프로세스가 자기 자신을 아직 선언 못 한 상태로 처음 스케줄링됨(부트스트랩 한계).
- CPUS=1(단일 코어)에서만 측정 — 멀티코어는 별도 측정 필요.
- "작업별 CPU 지분"(순간 배분 비율), "최대 runnable 대기시간"은 아직 계측 안 함(완료시간이라는 총량 지표만 있음).
- kerneltrap()은 여전히 무조건 yield — 정책이 전체 경로에 일관 적용 안 됨.

### 최종 측정 결과 (v2, 버그 수정 후)

워크로드: A(백그라운드 순수계산) 4개 각 6억 회, B(지연민감) 100라운드 각 1억 회, `DEADLINE_TICKS=15`, CPUS=1, 기본 TCG.

| | RR (기준선) | `PriorityPreempt<true>` |
|---|---|---|
| B p50 / p99 (tick) | 4 / 19 | 4 / **8** |
| 목표(15tick) 초과율 | 5/100 (5%) | **0/100 (0%)** |
| A 4개 완료 tick | 132, 132, 132, 141 | 184, 185, 193, 290 |
| A 완료까지 걸린 시간 | ≈115tick | ≈**263tick (약 2.3배)** |

**결론:** `PriorityPreempt<true>`는 B의 tail latency(p99)를 절반 이하로 줄이고 deadline 초과를 없애지만, 그 대가로 A의 완료 시간이 약 2.3배 늘어난다. p50은 거의 동일(4 vs 4) — 차이는 tail과 A 처리량에서만 나타난다. **"B가 가끔 온다면 A가 결국 끝나면서도 B의 꼬리 지연을 절반으로 줄일 수 있다"**는 정량화된 절충으로 표현 가능.

### 연구 질문 재정의 (다음 설계 가설, 미구현)

> 작업의 지연 요구를 우선순위에 반영하되, CPU 사용 예산과 대기 시간 보정을 어떻게 결합해야 tail latency를 낮추면서 다른 작업의 진행을 보장할 수 있을까?

세 축을 구분해서 설계: **우선순위**(얼마나 급한가, 명시적 선언) / **연속 실행 quantum**(한 번 선택됐을 때 얼마나 오래 도는가) / **누적 CPU budget**(일정 기간 얼마나 쓸 수 있는가, 소진 시 강등·보충 규칙 포함). quantum만 제한하면 strict priority 선택 때문에 Normal이 계속 밀릴 수 있어, 가중 CPU 지분/budget 소진 시 강등/aging 중 무엇으로 최소 진행을 보장할지가 다음 검증 대상.

## 교수님/타인에게 설명할 때 표현

- ❌ "C++로 대기 시간을 줄였다"
- ✅ "정책 비교 기반을 구현했고, 이를 통해 응답 시간·처리량·공정성의 절충을 연구하려 한다"
- 디스패치 실험: ❌ "정적 디스패치가 빠르다" → ✅ "간접 호출 비용과 컴파일러 최적화 효과를 분리해 분석했다"
- ADIOS 연결점: "busy-wait/yield를 선택하는 정책" 연구가 아니라 페이지폴트 핸들러+스케줄러를 같은 주소공간에 두고 **양보 비용 자체를 낮춰** 스케줄링 유불리를 바꾼 연구. 이 프로젝트와 맞닿는 지점은 "언제 CPU를 넘길지"와 "넘기는 비용이 지연에 주는 영향" — xv6의 tick 단위 실험을 ADIOS의 마이크로초 RDMA 환경에 직접 일반화할 순 없음, 연구 관심의 확장으로 설명.

## 파일 맵

| 파일 | 역할 |
|---|---|
| `kernel/sched_policy.hpp` | `SchedulerPolicy` concept (`pick_next` + `should_preempt`) |
| `kernel/sched_rr.hpp` / `sched_fcfs.hpp` | 기존 정책, 둘 다 `should_preempt`는 항상 true(행동 불변) |
| `kernel/sched_priority.hpp` | `PriorityPreempt<UseCounter>` — 이번 실험의 핵심 정책 |
| `kernel/scheduler.cpp` | `dispatch()`, `using ActivePolicy = ...`(정책 전환은 이 한 줄), `policy_should_preempt()` 경계 함수 |
| `kernel/trap.c:94-97` | usertrap()의 선점 훅 (kerneltrap()은 안 건드림) |
| `kernel/proc.h` / `proc.c` | `priority` POD 필드, `g_ls_runnable_count`, `setpriority` 반영 지점 |
| `kernel/syscall.h` / `syscall.c` / `sysproc.c` | `setpriority` 신규 syscall |
| `kernel/bench.cpp` | 디스패치 벤치마크(실험 1) — `run_dispatch_bench()`, `main.c`에서 호출 |
| `user/latencytest.c` | 실험 2의 워크로드+측정 드라이버 |
| `docs/bench/` | 실험 1의 원시 로그/역어셈블 |

## 재현 방법

```bash
# 빌드 (기본 CPUS=3)
make CPUS=1

# 정책 전환: kernel/scheduler.cpp의 이 한 줄만 바꾸고 재빌드
#   using ActivePolicy = RR;                  // 기준선
#   using ActivePolicy = PriorityPreempt<true>;  // O(1) 판단비용
#   using ActivePolicy = PriorityPreempt<false>; // O(n) 판단비용 (미측정)

# latencytest 실행 (CPUS=1 권장 — 단일 코어에서 경쟁을 최대화)
(sleep 3; printf 'latencytest\n'; sleep 300) | timeout 330 \
  qemu-system-riscv64 -machine virt -bios none -kernel kernel/kernel -m 128M \
  -smp 1 -nographic -global virtio-mmio.force-legacy=false \
  -drive file=fs.img,if=none,format=raw,id=x0 \
  -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
# 결과 나오면 즉시 프로세스를 kill할 것 -- sleep으로 오래 붙잡아두지 말 것.

# 회귀 확인 (매 변경 후 필수)
# usertests -q, mmaptest -- 둘 다 stdin에 앞서 `sleep 3` 정도 버퍼를 둬야
# 쉘이 준비되기 전에 입력이 씹히는 문제를 피할 수 있음(이번 세션에서 실제로 겪음).
```

## 다음 작업 순서

1. budget 축(누적 CPU 사용량, 소진 시 강등/aging) 추가한 새 정책 설계·구현.
2. `PriorityPreempt<false>`(O(n))로 판단비용 재측정, `UseCounter` 축 완성.
3. ✅ **완료 (2026-09-12, Codex 5라운드 감사 후 Implementation Gate `ACCEPTED`)**: 작업별 CPU 지분·최대 runnable 대기시간 계측 추가. `struct proc` POD 필드 4개(`sched_ready_tick`/`wait_ticks_total`/`wait_ticks_max`/`run_ticks_total`), 신규 syscall `sched_stats`(#25), 유저 도구 `schedstat`/`statstest`. 실제 WSL 빌드·`usertests -q` PASS ALL TESTS·자동화 테스트(`statstest`, 워커 3개 RR에서 `open_wait_ticks` 2/1/0 계단식 분포 확인)까지 완료. 원시 로그: `docs/bench/phase18b_usertests_raw_output.txt`, `docs/bench/phase18b_statstest_raw_output.txt`. 감사 과정에서 실제 결함 4건 발견·수정(락 순서/원자성, `ticks` 32비트 wraparound, `sys_sched_stats()` 샘플 시점 불일치, `kill` 직전 마지막 틱 미계상). 상세는 `agent-management/projects/xv6_os_project/DECISIONS.md` 2026-09-12 항목 참고.
4. 요청 도착률·burst·작업 길이 편차를 바꿔가며 재측정, "B 빈도 대비 A 기아 정도"의 임계점 정량화.
5. `should_preempt()`에 실행시간 상한 추가(LatencySensitive 자신도 무기한 보호받지 않도록).

## 이번 세션 전체 변경 파일 (미커밋, 베이스 커밋 대비)

```
 Makefile                |   2 +
 kernel/main.c           |   4 ++
 kernel/proc.c           |  12 ++++
 kernel/proc.h           |   2 +
 kernel/riscv.h          |   7 ++-
 kernel/sched_fcfs.hpp   |   4 ++
 kernel/sched_policy.hpp |   8 ++-
 kernel/sched_rr.hpp     |   4 ++
 kernel/scheduler.cpp    |  29 ++++++++-
 kernel/syscall.c        |   2 +
 kernel/syscall.h        |   1 +
 kernel/sysproc.c        |  15 +++++
 kernel/trap.c           |   7 ++-
 plan.md                 | 160 ++++++++++++++++++++++++++++++++++++++++++++++--
 user/user.h             |   1 +
 user/usys.pl            |   1 +
 (신규) docs/bench/*, kernel/bench.cpp, kernel/sched_priority.hpp, user/latencytest.c, HANDOFF.md
```
