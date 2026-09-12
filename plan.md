# xv6 + Modern C++ 최적화 프로젝트 계획서

## 요약 (Phase별 한 줄)

| Phase | 상태 | 한 줄 요약 |
|---|---|---|
| 0 | ✅ 완료 | `mcounteren` CSR 활성화 + `r_cycle`/`r_instret` 추가로 이후 모든 실측의 기반을 만든다 |
| 0.5 | ✅ 완료 | GCC 내장 `-fanalyzer`를 정적 분석으로 채택(설치 불필요, 기존 코드 클린 확인), C++20 모듈은 이 툴체인 미지원으로 실측 후 기각 |
| 1 | 🟡 FCFS+RR 완료 | RR/FCFS를 concepts+템플릿 정적 디스패치로 구현해 함수포인터 대비 명령어 수 감소를 실측 증명한다 — 이 프로젝트의 핵심 |
| 1.5 | ✅ 완료 | `sys_mmap`을 C++로 포팅해 매크로/필드순서/인덱스루프 버그를 컴파일 에러로 격상시킨다 |
| 1.8 | 🟡 설계 확정(범위 재조정), 구현 시작 전 | **현재 연구 목표.** RR 대비 우선순위 기반 선점 정책 하나만 비교 — 긴 배경계산 A + 지연민감 요청 B 두 종류로 좁힘. 목표 응답시간 초과율·A의 진행·추가 판단비용을 측정 |
| 1.6 | 부분 완료(설계만) | `ErrorOr<T>`/`TRY()`로 균일한 에러 전파를 만든다 (`RefPtr`을 구조체 필드로 넣는 안은 기각됨) |
| 1.7 | 미착수 (설계만) | Stroustrup 2025 논문의 `Number<T>` 패턴을 이식해 Phase 1.5에서 우연히 잡은 narrowing 버그를 프로젝트 전체 차원에서 체계적으로 방지한다 |
| 2 | ⏸️ 보류 (설계만, 의무 아님) | CFS 정렬 자료구조를 힙 할당 없는 인트루시브 템플릿 트리로 구현한다 — Phase 1.8이 RR 대비 단일 비교로 범위를 좁히면서 후순위로 밀림. 설계(`project1-braeden-hue`의 RB tree 포팅안)는 보존 |
| 2.5 | 미착수 (Phase 2 선행) | Phase 2 트리를 증강해서 mmap 주소충돌 회피를 O(N²)→O(log N)으로 개선한다 (단, `NVMA=16`이라 실측 이득은 제한적) |
| 3 | 미착수 | CAS 기반 락-프리 상태전이 + xv6 `sleep`/`wakeup`을 결합해 broadcast-wake 비효율을 줄인다 |
| 4 | 미착수 | `std::atomic` refcount로 COW fork의 락 세분화를 증명한다 — 정적 디스패치와 무관한 별도 동시성 축 |
| 5 | 미착수 | 지금까지의 실측 결과를 벤치마크/문서로 정리해 포트폴리오 산출물로 묶는다 |

## 0. 목적

이 저장소는 MIT 6.1810 기반 xv6-riscv (HYU-ELE3021 과제)이며, **mmap(project3)은 순수 C로 이미 완료·제출된 상태**다 ([kernel/sysfile.c](kernel/sysfile.c)의 `sys_mmap`/`sys_munmap`/`mmapfault`).

이 프로젝트의 목적은 "OS 기능을 더 만든다"가 아니라, **C++20/23의 특정 기능(concepts, templates, constexpr)이 커널 hot path에서 런타임 오버헤드를 실측 가능한 수준으로 줄인다는 것을 증명**하는 것이다. 그래서 대상 선정 기준은 다음 두 조건을 모두 만족하는 지점이다:

1. **여러 개의 교체 가능한 전략(알고리즘)이 존재**해야 한다 — 정적 디스패치의 이득은 "선택지가 여럿"일 때만 의미가 있다.
2. **I/O를 타지 않는 CPU 바운드 hot path**여야 한다 — 디스크 I/O가 섞이면 명령어 수 차이가 지연시간에 묻혀 측정 불가능하다.

mmap은 이미 완성됐고 두 조건 다 만족하지 않는다(전략이 하나뿐, I/O 바운드) — 그래서 **"정적 디스패치로 속도를 증명한다"는 명제의 베이스라인으로는 손대지 않는다.** 대신 mmap은 **다른 명제(타입 안전성/자원 안전성)의 증명 무대**로 쓴다 (Phase 1.5). 스케줄러(RR/FCFS/CFS 여러 전략, 타이머 인터럽트마다 실행되는 순수 CPU 코드)는 여전히 "속도" 명제의 1순위 대상이다.

이 프로젝트 전체는 Bjarne Stroustrup이 반복적으로 제시하는 Contemporary C++의 5대 특성 — **정적 타입 안전성(static type safety), 자원 안전성(resource safety, no leak), 하드웨어 직접 제어(direct mapping to hardware), 효율성(zero-overhead efficiency), 안정성/호환성(stability)** — 을 각 Phase가 하나 이상 실측으로 증명하는 구조로 짠다. 전체 매핑은 §6 참고.

## 1. 확정된 환경

- **1차 문헌**: Bjarne Stroustrup, *"Concept-Based Generic Programming in C++"* (Columbia University, 2025, `Concept-based GP.pdf`). Phase 1의 "정적 디스패치가 런타임 오버헤드를 없앤다"는 이 프로젝트의 중심 명제를 저자 본인이 §7.2에서 직접 서술함(GP=컴파일타임 정적 다형성+인라인 가능 vs OOP=vtable 간접호출+힙 할당 경향). §4.2~4.3의 `Sortable_range` concept 설계 패턴을 Phase 1의 `SchedulerPolicy` concept 구현 시 그대로 참고할 것. §2의 `Number<T>` narrowing 방지 패턴은 Phase 1.7로 별도 반영.
- 크로스 툴체인: `riscv64-linux-gnu-gcc-14` / `riscv64-linux-gnu-g++-14` (14.2.0, [Makefile:58-64](Makefile:58) `GCC_VER = -14`로 고정, `make GCC_VER=`로 롤백 가능)
- 언어 표준: C++23 (`CXXFLAGS`에 `-std=c++23 -fno-exceptions -fno-rtti -fno-threadsafe-statics`, [Makefile](Makefile) 참고)
- **알려진 제약**: `deducing this`는 GCC 14부터 지원(GCC 13 불가, 실측 확인됨). freestanding이라 STL 컨테이너/예외/RTTI 없음 — 커널 코드는 concepts/templates/constexpr/span처럼 헤더온리·런타임서포트 불필요한 기능만 사용.
- **⚠️ 아키텍처 규칙 (Phase 1.6 검토 중 발견, 2026-08-25): 공유 C 구조체엔 POD 필드만.** `struct proc`/`struct vma`([kernel/proc.h](kernel/proc.h))는 `trap.c`/`sysproc.c`/`exec.c`/`file.c`/`pipe.c` 등 커널 거의 전체가 include한다. 여기에 생성자/소멸자 있는 C++ 클래스 타입 필드(`RefPtr`, `std::atomic` 등)를 추가하면 그 순간 `proc.h`를 include하는 모든 `.c` 파일이 파싱 자체가 안 돼서, 사실상 커널 전체를 C++로 포팅해야 하는 비현실적 상황이 된다(`RefPtr`-in-`struct vma` 시도로 실측 확인, Phase 1.6 참고). **공유 구조체엔 항상 POD 타입(int/포인터)만 넣고, C++ 스마트 타입은 함수 지역변수로만 사용한다.** Phase 2의 트리 노드 필드는 POD라 안전하지만, **Phase 3의 CAS 대기 상태 설계 시 반드시 이 규칙을 적용할 것**(`std::atomic<int>`를 필드로 넣지 말 것). Phase 1의 `arrival_seq`(uint64)도 이 규칙을 따름 — 실제로 적용된 첫 사례.
- **커널 카운터 CSR**: [kernel/start.c](kernel/start.c)가 `mcounteren`에 TIME 비트(2)만 켜놨었음. `rdcycle`/`rdinstret`을 커널(S-mode)에서 쓰려면 **CY+IR 비트도 켜야 함** — Phase 0에서 완료.
- **실측으로 확정된 freestanding 가용성** (동일 빌드 플래그로 직접 컴파일 검증, 2026-08-25~27):
  | 기능 | freestanding(`-ffreestanding -nostdlib`)에서 | 근거 |
  |---|---|---|
  | `std::atomic<T>::compare_exchange_weak/fetch_add` 등 CAS류 | ✅ 사용 가능 | AMO/LR-SC 명령어로 직접 코드생성, 미해결 심볼 0개 |
  | `std::atomic<T>::wait/notify_one` | ❌ 사용 불가 | 헤더에서 멤버 자체가 선언 안 됨(hosted 스레드 지원 필요) — Phase 3는 이 발견으로 설계 변경됨 |
  | `std::bitset` | ✅ 사용 가능 | 컴파일·상수폴딩 확인, 다만 Phase 0엔 `enum class` 방식을 채택(이름 기반이 이 케이스에 더 적합) |
  | `deducing this` | GCC 14부터 | GCC 13.3은 컴파일 에러 |
  | `RefPtr<T,IncRef,DecRef>` (함수포인터 비타입 템플릿 인자) + `ErrorOr<T>`/`TRY()` | ✅ 사용 가능(지역변수로만) | 실측 컴파일 클린, 미해결 심볼 0개. 단, `TRY()`는 GCC/Clang statement expression(`({ ... })`) 확장에 의존 — ISO 표준 아님, 이 프로젝트는 gcc/g++ 전용이라 문제 없지만 이식성 가정 시 주의 |
  | C++20 Modules (`export module`/`import`) | ❌ 사용 불가 | `.cppm`을 `-fmodules-ts`로 컴파일해도 조용히 아무 산출물 없음, `-x c++-module` 명시 시 `error: language c++-module not recognized` — 이 크로스 g++-14 빌드는 모듈 미지원 (Phase 0.5) |
  | GCC 내장 `-fanalyzer` | ✅ 사용 가능(설치 불필요) | Phase 0/1.5 코드에 실측 적용, findings 0개 (Phase 0.5) |
  | `<concepts>` (`std::same_as` 등) | ✅ 사용 가능 | Phase 1 `SchedulerPolicy` concept 구현에 실측 사용, 컴파일 클린 |

## 2. 모듈별 계획

### Phase 0 — 벤치마크 인프라 (선행 작업, 다른 모든 Phase의 전제)

**✅ 구현 완료 (2026-08-25):** [kernel/riscv.h](kernel/riscv.h)에 `r_cycle()`/`r_instret()` 추가(기존 `r_time()` 바로 옆, 동일한 `static inline uint64 r_xxx() { asm volatile("csrr %0, <name>" : "=r"(x)); return x; }` 패턴). [kernel/start.c](kernel/start.c)를 [kernel/start.cpp](kernel/start.cpp)로 전환하고 `enum class Mcounteren`으로 CSR 마스크를 표현(매직넘버 `0b111` 대신):
```cpp
enum class Mcounteren : uint64 { CY = 1u<<0, TM = 1u<<1, IR = 1u<<2 };
constexpr Mcounteren operator|(Mcounteren a, Mcounteren b) {
    return static_cast<Mcounteren>(static_cast<uint64>(a) | static_cast<uint64>(b));
}
constexpr uint64 raw(Mcounteren m) { return static_cast<uint64>(m); }
// timerinit() 안에서:
w_mcounteren(r_mcounteren() | raw(Mcounteren::CY | Mcounteren::TM | Mcounteren::IR));
```

`start.c` 전체를 C++로 바꾼 이유: `enum class`를 쓰려면 C++이어야 하는데, 검토 결과 `start.c`는 `struct proc`/`struct vma`를 전혀 안 건드리는 순수 CSR 조작 코드라(§1의 POD-only 규칙과 무관) 안전하게 통째로 포팅 가능했다. 실제로 부딪힌 것:
- `entry.S`가 `call start`로 직접 부르고 `main.c`가 `void main(void)`를 정의하므로, `start.cpp`의 **전방선언과 정의 둘 다** `extern "C"`가 필요함 — 하나라도 빠지면 `conflicting declaration ... with 'C' linkage` 컴파일 에러 (실제로 재현하고 고침).
- `-ffreestanding`에서는 `main`이라는 이름에 대한 hosted-구현 특수 규칙(참조/호출 금지)이 적용되지 않아서 `extern "C" void main();`을 자유롭게 선언·호출 가능함을 확인.
- `enum class Mcounteren` + `constexpr operator|`가 실제로 `ori a5,a5,7` **명령어 하나**로 완전히 상수 폴딩됨을 objdump로 확인.
- `timerinit()`은 `start.c` 내부에서만 쓰이는 private 함수라(grep 확인) 파일 전체를 옮겨도 외부 링크 걱정이 없음.

전체 커널 clean rebuild 성공, `nm`으로 `start`/`timerinit` 심볼이 mangling 없이 노출됨 확인, `mmaptest` 재통과로 부팅/타이머 회귀 없음 확인. **추가로 Phase 0의 진짜 목적(S-mode에서 `cycle`/`instret`을 실제로 읽을 수 있는가)까지 검증**: `kernel/main.c`에 `r_cycle()`/`r_instret()`을 부팅 배너에서 호출하는 임시 코드를 넣어 QEMU로 실행 — 트랩/패닉 없이 정상 출력됨을 확인하고 원복(`[phase0 check] r_cycle=... r_instret=...` 출력, 이후 정상 부팅 계속됨).

측정 규칙 확정(이후 모든 Phase에 동일 적용):
- 실제 인터럽트 핸들러 통째로 재지 말고 **디스패치 호출 지점만** 분리한 타이트 루프로 N회(수만~수십만) 반복
- 루프는 `intr_off()`/`intr_on()`으로 감싸 인터럽트 잡음 차단
- **첫 반복 폐기** (QEMU TCG의 TB 최초 번역 비용이 섞임)
- 평균이 아니라 **중앙값 + 최솟값** 리포트
- `rdcycle`과 `rdinstret`을 항상 같이 재고, 둘이 거의 같게 나오는 걸 "QEMU TCG는 타이밍 모델이 없다"는 확인으로 명시 (성공적 측정 실패가 아님)
- 정적 증거는 `objdump -d`로 디스패치 호출 지점 diff (간접 `ld`+`jalr` vs 직접 `jal`)

**✅ `rdinstret` 캘리브레이션 완료, 중대 발견 (2026-09-XX, [kernel/bench.cpp](kernel/bench.cpp)):** 정확히 `1+2n` 명령어를 실행하는 타이트 루프(`mv`+`n`회의 `addi`/`bnez`)로 `rdinstret` 델타를 실측했다.
- **기본 QEMU 모드(`-icount` 없음)에서는 `rdinstret`이 신뢰 불가능하다.** 같은 코드를 20번 반복 실행해도 기댓값(2001)으로 수렴하지 않고 **약 1.8배(3656~3688)에서 플래토**를 형성한다. `objdump`로 생성된 어셈블리가 `rdinstret`/`mv`/`addi`/`bnez`/`rdinstret`만 정확히 있고 다른 메모리 접근이 안 끼어있음을 확인했으므로(코드 버그 아님), **QEMU TCG의 기본 `instret` 구현이 "실제 retired 명령어 1개당 1 증가"가 아니라 다른 단위(TCG 내부 micro-op 추정)로 세고 있다는 뜻**이다 — 팀원이 지적한 "카운터 의미를 확인해야 한다"가 실측으로 확인된 사례.
- **`-icount shift=0`로 부팅하면 완벽하게 정확해진다.** 20번 반복 전부 오차 없이(`+1`의 상수 오프셋만, 측정 경계 자체에서 나오는 것으로 설명 가능) 정확히 일치했고, `slope` 체크(1000→2000, 2000→4000)도 정확히 2000/4000으로 맞았다.
- **결론: 이후 모든 `rdcycle`/`rdinstret` 실험은 `-icount shift=0`로 QEMU를 부팅해야 한다.** 기본 `make qemu`(Phase 0의 CY/IR 활성화 sanity check 등)는 "트랩 없이 읽힌다"는 확인에는 문제없지만, 정확한 수치 비교에는 부적합하다는 게 이번에 새로 밝혀졌다.
- 부팅 커맨드: `qemu-system-riscv64 ... -icount shift=0 ...` (`Makefile`의 `qemu` 타깃엔 아직 반영 안 함 — 벤치마크 전용 실행 시 수동으로 추가)

### Phase 0.5 — 정적 분석 인프라 (`-fanalyzer` 채택, Modules는 실측 후 기각)

**✅ 구현 완료 (2026-08-27):** [Makefile](Makefile)에 `CXXFLAGS_ANALYZE`(`CXXFLAGS` + `-fanalyzer`)와 `analyze` 타깃 추가. `make analyze` 실행 결과 신규 `.cpp` 파일 전부(당시 `start.cpp`, `sysfile_mmap.cpp`, 이후 `sched_fcfs.cpp`도 자동 포함) findings 0개로 통과 확인. `$(wildcard $K/*.cpp)`로 대상 파일을 자동 탐색하므로 향후 파일이 추가돼도 별도 수정 없이 자동으로 검사 대상에 포함됨.

**Modules — 실측 후 기각:** C++20 모듈(`export module`, `import`)을 이 프로젝트의 정확한 크로스 툴체인(`riscv64-linux-gnu-g++-14`)으로 직접 시도했다. `.cppm` 파일을 `-std=c++20`/`-std=c++23` + `-fmodules-ts`로 컴파일하면 **아무 산출물도 없이 경고만 뜨고 조용히 끝나고**(`warning: ... linker input file unused because linking not done`), `-x c++-module`로 언어 모드를 명시하면 `error: language c++-module not recognized`로 명확히 거부된다. **이 크로스 컴파일러 빌드는 모듈을 지원하지 않는다** — 채택하지 않고 기존 방식(헤더 + `extern "C"` 경계)을 유지한다. Stroustrup 논문 §6.6도 모듈의 주 동기가 "모듈성 + 컴파일 속도"라고 명시하는데, 이 프로젝트는 `.cpp` 파일이 10개 미만이라 그 이득도 원래 작다 — 기각을 이중으로 뒷받침.

**Makefile 통합:**
```makefile
CXXFLAGS_ANALYZE = $(CXXFLAGS) -fanalyzer
analyze:
	@for f in $(wildcard $K/*.cpp); do \
		echo "== $$f =="; \
		$(CXX) $(CXXFLAGS_ANALYZE) -fsyntax-only -I. $$f; \
	done
```

**C++ Core Guidelines / "Profiles"에 대한 정직한 평가:** Stroustrup·Sutter가 C++26을 목표로 제안 중인 컴파일러 강제 "Profiles"(bounds/lifetime/type safety, 미국 정부의 메모리 안전성 요구와 맞물려 현재 활발히 논의 중)는 **GCC/Clang 어디에도 아직 구현되지 않은 미래 기능**이라 지금 적용 불가능하다 — 억지로 흉내내지 않는다. 다만 이 프로젝트가 이미 실천 중인 것들(Phase 1.5 designated init, Phase 1.6 RAII/`ErrorOr`, Phase 1.7 `Number<T>`, Phase 2 zero-allocation)이 정확히 Profiles가 강제하려는 세 축(bounds/lifetime/type)과 일치한다 — "표준 기능이 나오기 전에 같은 원칙을 수작업으로 먼저 실천했다"는 서사로 포트폴리오에 명시.

**Contemporary C++ 특성 매핑:**
- ✅ 정적 타입 안전성: `-fanalyzer` 검증을 모든 신규 `.cpp` 파일에 적용하는 걸 앞으로의 관례로 삼음
- 해당 없음: 나머지 특성(이 Phase는 도구 채택이지 기능 구현이 아님)

### Phase 1 — 스케줄러 정책 모듈화 (핵심 프로젝트)

**✅ FCFS + RR 완료 (2026-08-27):** `SchedulerPolicy` concept·`dispatch()` 배관·정책 두 개(FCFS, RR)까지 실제로 동작. [kernel/proc.c](kernel/proc.c)의 `scheduler()`는 `scheduler_dispatch()` 한 줄만 호출. `struct proc`에 POD 필드 `arrival_seq`(uint64) 추가하고 `mark_runnable()` 헬퍼로 5개 RUNNABLE 전환 지점(`userinit`/`fork`/`yield`/`wakeup`/`kill`)에서 통일되게 스탬프.

**⚠️ 구조 변경 (RR 추가하며 발견): 정책 struct와 dispatch 배관을 분리.** FCFS만 있을 땐 `kernel/sched_fcfs.cpp` 하나에 `FCFS` struct + `dispatch()` + `scheduler_dispatch()`가 전부 같이 있었다. RR을 똑같은 방식으로 `sched_rr.cpp`에 추가하고 `OBJS`에 얹으면, 두 파일이 각자 `scheduler_dispatch`를 정의해서 **링크 단계에서 중복 심볼 에러**가 난다. 그래서 재구성했다:
- `kernel/sched_fcfs.hpp`, `kernel/sched_rr.hpp` — 정책 struct **정의만** 담은 헤더(`#pragma once`)
- [kernel/scheduler.cpp](kernel/scheduler.cpp) — `dispatch()` 템플릿 + `using ActivePolicy = ...` + `extern "C" scheduler_dispatch()`, 정책 헤더 두 개를 동시에 include

이 구조면 두 정책 헤더를 항상 같이 include해놔도(나중에 CFS까지) 실제로 컴파일되는 건 `using ActivePolicy`로 고른 것 하나뿐이다 — 템플릿 인스턴스화가 게으르기 때문에, 안 쓰는 정책은 바이너리에 아예 안 남는다. **실측 확인**: `nm kernel/kernel`으로 확인한 결과 `FCFS` 관련 심볼은 바이너리에 전혀 없고, `RR::pick_next`조차 별도 심볼로 안 남을 만큼 `dispatch<RR>` 안에 완전히 인라인됐다 — "정책 전환은 한 줄, 죽은 정책은 코드생성 자체가 안 됨"이 이론이 아니라 실측으로 확인된 순간.

**RR 구현 중 실제로 잡은 버그 2개 (사용자가 직접 작성 후 리뷰):**
1. `(last + i) / n`을 `%`로 착각 — 나눗셈이라 `NPROC=64`일 때 대부분 `idx`가 0으로 뭉개져서 사실상 항상 `proc[0]`만 보는 코드가 됨. `%`로 수정.
2. `for (int i = 1; i < n; ...)` — `i <= n`이어야 `last` 자기 자신도 한 바퀴 끝에 재검사됨. `i < n`이면 RUNNABLE 프로세스가 단 하나(그것도 `last` 자신)일 때 영원히 못 찾고 `nullptr`을 리턴 — 실행 가능한 프로세스가 있는데도 스케줄러가 멈추는 실제 starvation 버그.

```cpp
// kernel/sched_policy.hpp
#pragma once
#include <concepts>
extern "C" {
#include "types.h"
#include "proc.h"
}
template<typename P>
concept SchedulerPolicy = requires(P policy, struct proc* procs, int n) {
    { policy.pick_next(procs, n) } -> std::same_as<struct proc*>;
};
```

```cpp
// kernel/sched_fcfs.hpp -- 정책 struct만, dispatch 배관 없음
#pragma once
extern "C" { #include "types.h" #include "proc.h" }
#include "sched_policy.hpp"

struct FCFS {   // 무상태: 자기 안에 아무것도 기억 안 함
    struct proc* pick_next(struct proc* procs, int n) {
        struct proc* candidate = nullptr;
        for (int i = 0; i < n; i++) {
            struct proc* p = &procs[i];
            if (p->state == RUNNABLE) {
                if (!candidate || p->arrival_seq < candidate->arrival_seq)
                    candidate = p;
            }
        }
        return candidate;
    }
};
```

```cpp
// kernel/sched_rr.hpp -- 정책 struct만
#pragma once
extern "C" { #include "types.h" #include "proc.h" }
#include "sched_policy.hpp"

struct RR {   // 유상태: last를 기억해야 원형으로 공평하게 순회됨
    int last = -1;
    struct proc* pick_next(struct proc* procs, int n) {
        for (int i = 1; i <= n; i++) {          // <= n: last 자신도 재검사
            int idx = (last + i) % n;           // %, 나눗셈 아님
            struct proc* p = &procs[idx];
            if (p->state == RUNNABLE) { last = idx; return p; }
        }
        return nullptr;
    }
};
```

```cpp
// kernel/scheduler.cpp -- dispatch 배관 + 정책 선택, 여기만 유일하게 scheduler_dispatch()를 정의
extern "C" {
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"
}
#include "sched_policy.hpp"
#include "sched_fcfs.hpp"
#include "sched_rr.hpp"

template<SchedulerPolicy P>
[[noreturn]] static void dispatch(P& policy)
{
    struct cpu* c = mycpu();
    c->proc = 0;
    for (;;) {
        intr_on();
        intr_off();
        struct proc* next = policy.pick_next(proc, NPROC);
        if (next) {
            acquire(&next->lock);
            if (next->state == RUNNABLE) {
                next->state = RUNNING;
                c->proc = next;
                swtch(&c->context, &next->context);
                c->proc = 0;
            }
            release(&next->lock);
        } else {
            asm volatile("wfi");
        }
    }
}

using ActivePolicy = RR;   // 이 한 줄로 정책 선택 (FCFS로 되돌리려면 이 줄만 교체)

extern "C" [[noreturn]] void scheduler_dispatch(void)
{
    static ActivePolicy policy;
    dispatch(policy);
}
```

빌드 중 실제로 부딪힌 것들 (FCFS만 있었을 때, Phase 1 최초 착수 시):
- `kernel/proc.h`에 `#pragma once`가 없어서 `sched_policy.hpp`와 정책 파일이 각자 `proc.h`를 include하니 재정의 에러 — 원본 C는 각 `.c`가 헤더를 한 번만 include하는 관례라 안 걸렸던 문제. `proc.h`에 `#pragma once` 추가로 해결(C++ 파일들이 헤더를 조합해서 쓰기 시작하면서 처음 필요해진 것).
- `extern struct proc proc[NPROC];`를 `struct proc` 정의보다 앞에 선언해서 "incomplete type" 에러 — 배열은 원소 타입이 완전해야 크기를 알 수 있음. `struct proc` 정의 뒤로 이동.
- `scheduler()`가 `__attribute__((noreturn))`인데 본문을 `scheduler_dispatch()` 호출로 바꾸니 "noreturn 함수가 리턴한다" 에러 — `scheduler_dispatch`와 그 안의 `dispatch()` 템플릿 둘 다 `noreturn`으로 표시해야 컴파일러가 체인 전체가 안 돌아온다는 걸 앎.

전체 커널 clean rebuild 성공(FCFS 단독일 때, 그리고 RR로 재구성한 뒤 다시), `nm`으로 `scheduler_dispatch`/`mark_runnable` 심볼 확인, **`usertests -q` 전체 통과(`PASS ALL TESTS`)**와 `mmaptest` 전체 통과를 FCFS/RR 각각에서 QEMU 재확인 — fork/exit/wait/pipe/sleep/wakeup/kill처럼 스케줄러를 강하게 타는 경로가 두 정책 모두에서 문제없이 동작함을 실측 확인. `nm`으로 바이너리엔 `FCFS` 관련 심볼이 전혀 없고 `RR::pick_next`조차 `dispatch<RR>`에 완전히 인라인된 것도 확인 — "안 쓰는 정책은 코드생성이 아예 안 된다"는 실측 증거. 다음은 CFS(정책 인터페이스 + Phase 2 트리 결합) → Phase 0의 rdcycle/rdinstret 벤치마크로 FCFS/RR/CFS 실측 비교.

**✅ 정적(템플릿) vs 함수포인터 디스패치 비용 실측 완료 (2026-09-11, [kernel/bench.cpp](kernel/bench.cpp)):**

*Baseline 설계(사용자 확정):* C++ 언어 자체는 고정하고 디스패치 메커니즘만 바꾼다 — `struct RR`(정적/템플릿, `sched_rr.hpp`와 동일 본체)와 동일 로직을 `static`(내부 링크) C++ 함수로도 작성해 `volatile` 함수 포인터(`pick_next_fn volatile g_rr_fn`)로 호출한다. `volatile`이 핵심: 단일 대입되는 전역 함수 포인터는 `-O`가 역가상화(devirtualize)해서 직접 호출로 되돌리는 전형적 패턴이라, `volatile`로 매번 실제 재적재+`jalr`을 강제했다(그렇지 않으면 fnptr 쪽도 인라인되어 비교 자체가 무의미해짐). 두 메커니즘 모두 동일한 합성 `bench_procs[64]` 배열(전역 `proc[]`는 건드리지 않음), 동일 초기 상태(`last=-1`, 마지막 슬롯만 RUNNABLE → 매 호출 64칸 풀스캔이 되는 결정론적 최악 경우)에서 실행했고, 측정 구간은 `r_instret()` 전후로 `pick_next()` 호출만 감싸 `dispatch()` 루프 전체가 아닌 순수 호출만 격리했다. `CPUS=1`, `-icount shift=0`(§0의 실측 검증된 규칙)로 부팅.

정책 본체와 호출 경로 비용을 분리하기 위해 O(1) trivial 본체(`return &procs[0];`)도 같은 두 메커니즘으로 먼저 측정했다. 200회 반복, 결과(instret/call):

| 실험 | static(템플릿) | fnptr(간접) | 차이(fnptr − static) |
|---|---|---|---|
| trivial 본체 (O(1)) | 2 | 14 | **+12** |
| RR 본체 (O(64) 스캔, n=64 상수) | 707 | 533 | **−174** (역전!) |
| RR 본체, n을 `volatile int`로 불투명화 | 517 | 533 | **+15** |

*trivial 본체:* `objdump`로 확인한 결과 static 루프는 몸체가 **완전히 사라지고**(순수 함수 + 상태 불변 → 컴파일러가 계산을 통째로 소거, 루프 제어만 남음 — 2 instret/call은 `addiw`+`bnez` 그 자체) fnptr 루프는 매 반복 `ld`(포인터 재적재)+`mv`+`mv`+`jalr`(간접 호출) 후 콜리(callee)가 실제로 주소를 계산해 반환 — 간접 호출이 어셈블리에 실질적으로 남아있음을 확인. **정적 디스패치의 "호출 경로" 비용은 사실상 0, 함수 포인터는 call당 +12 instret의 고정 세금.**

*RR 본체 역전의 원인 (실측·objdump로 규명):* static 버전은 `dispatch<RR>()`와 동일하게 `pick_next(bench_procs, BENCH_NPROC)`을 호출하는데 `BENCH_NPROC`이 컴파일타임 상수(64)라 GCC가 인라인 후 `(last+i) % 64`의 나눗셈을 **부호 있는 정수의 2의 거듭제곱 나눗셈용 소프트웨어 시퀀스**(`sraiw`+`srliw`+`addw`+`andi`+`subw`, 5개 명령)로 강도 감소(strength reduction)시켰다. 반면 `fnptr_rr_pick_next`는 독립 함수라 `n`이 런타임 매개변수로 남고, GCC는 그냥 **하드웨어 `remw`(M-extension) 명령 1개**를 냈다 — `-O`(=`-O1`) 수준에서 이 상수-나눗셈 강도감소가 오히려 하드웨어 나머지 연산보다 느린, 실제로 손해인 케이스였다. 이건 정적 디스패치의 결함이 아니라 **"인라인이 컴파일러에게 n=64라는 걸 보여줘서 생긴 별개의 코드생성 효과"**임을 증명하기 위해 세 번째 측정을 추가했다: `n`을 `volatile int` 전역으로 읽어 상수 전파를 원천 차단하면(그래도 여전히 정적/인라인 디스패치) static이 다시 517 instret/call로 fnptr(533)을 앞섰고, 그 격차(+15/call)는 trivial 본체의 순수 호출-경로 세금(+12/call)과 거의 일치한다.

**결론:** 호출 경로(call path) 비용은 정적 디스패치가 함수 포인터 대비 안정적으로 약 12~15 instret/call 더 싸다(간접 적재+`jalr`+콜리 프롤로그/에필로그 vs 완전 소거 또는 직접 인라인). 정책 본체(policy body) 비용 자체는 두 메커니즘이 동일한 로직을 실행하면 동일해야 하며, 실제로 `n`의 상수성을 통제하면 동일한 결과가 나온다 — 처음 관찰된 "정적 버전이 174 instret/call 더 느림"은 디스패치 메커니즘의 차이가 아니라 **GCC `-O1`이 인라인된 상수-나눗셈 강도감소를 하드웨어 `remw`보다 손해 보는 방향으로 선택한 컴파일러 특이 현상**이었다 — 이것도 실측 없이는 놓쳤을 정직한 결과라 그대로 남긴다. `dispatch<P>()`가 `NPROC`(매크로 상수)로 `pick_next`를 호출하는 실제 프로덕션 코드에서도 이 강도감소가 동일하게 일어나므로, 실서비스 빌드에도 그대로 적용되는 관찰이다(벤치마크만의 인공물이 아님).

**⚠️ 부수적으로 발견·수정한 실제 커널 버그 (인터럽트 상태 관리):** 위 벤치마크를 `main()`에 처음 연결했을 때, 출력은 정상적으로 찍히는데 그 직후 `kinit()` 어디선가 **패닉도 없이 조용히 멈추는** 문제가 발생했다. 이분 탐색(각 측정 호출을 하나씩 빼며 재현)으로 원인을 `run_dispatch_bench()`가 측정 구간을 감싸려고 넣었던 `intr_off()`/`intr_on()` 호출 자체로 좁혔다 — 측정 로직(트리비얼/RR 본체, 리셋, 루프)은 전혀 무관했고, `intr_off(); intr_on();`만 남기고 나머지를 다 지워도 재현됐다.

원인: [kernel/start.cpp](kernel/start.cpp)의 `timerinit()`은 `main()`이 시작되기 전 **M-mode에서 이미** `sie.STIE`/`sie.SEIE`를 언마스크하고 첫 타이머 인터럽트를 `w_stimecmp(r_time()+1000000)`로 예약해 둔다. 반면 `stvec`(트랩 벡터)는 `trapinithart()`가 설치하는데, 이 함수는 `main()`에서 `kinit()` **이후에** 호출된다. 즉 `main()` 진입 시점엔 개별 인터럽트 소스는 이미 언마스크되어 있고 타이머도 이미 예약되어 있지만, `sstatus.SIE`(전역 인터럽트 허용 비트)만 0이라 실제로는 아무 트랩도 발생하지 않는 안전한 상태다. 여기서 `run_dispatch_bench()`가 (측정 구간을 보호한다는 명목으로) `intr_on()`을 호출해 `sstatus.SIE`를 1로 올리면, 이미 예약된 타이머 인터럽트가 실제로 발동할 수 있게 되고, 발동 시점엔 `stvec`이 아직 미설정(쓰레기 값)이라 **CPU가 정의되지 않은 주소로 트랩되어 그대로 멈춘다** — 진짜 트랩 핸들러를 거치지 않으므로 `panic()` 출력조차 없다.

**수정:** `run_dispatch_bench()`/`run_calibration()` 양쪽에서 `intr_off()`/`intr_on()` 호출을 완전히 제거했다. 이 함수들은 `trapinit()` 이전에만 호출되므로 `sstatus.SIE`는 애초에 이미 0(비활성)이라 굳이 끌 필요가 없고, 켜는 순간이 바로 버그였으므로 켜지 않는 게 맞는 수정이다. 수정 후 벤치마크 수치는 완전히 동일하게 재현됐고(측정 로직 자체는 건드리지 않았으므로 당연함), `-icount shift=0` 부팅에서도 `usertests -q`/`mmaptest`까지 문제없이 이어짐을 확인했다. **교훈: `trapinithart()` 이전 구간에서 절대 `intr_on()`을 호출하지 않는다** — 이후 Phase(CFS 등)에서 부팅 초기에 측정/디버그 코드를 넣을 때도 이 불변조건을 지켜야 한다.

**재현 정보 (2026-09-11 최종 확정):**
- 베이스 커밋: `780da055fab1dd1adee2b8e2f188ddb0c7326642`("Modern C++ scheduler policies (FCFS/RR, CFS in progress) + mmap C++ port") 위에 이 세션의 변경(`kernel/bench.cpp` 신규, `kernel/main.c`/`kernel/riscv.h`/`Makefile`/`plan.md` 수정)이 아직 커밋되지 않은 상태로 얹혀 있음.
- 컴파일러: `riscv64-linux-gnu-g++-14`/`riscv64-linux-gnu-gcc-14` (Ubuntu 14.2.0-4ubuntu2~24.04.1) 14.2.0. 호스트: Ubuntu 24.04.4 LTS.
- QEMU: `QEMU emulator version 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.16)`.
- 디스패치 벤치마크(신뢰 가능한 수치) 부팅 옵션: `-machine virt -bios none -kernel kernel/kernel -m 128M -smp 1 -nographic -icount shift=0 -global virtio-mmio.force-legacy=false -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0` (CPUS=1 고정 — §0에서 확정한 대로 `-icount shift=0` 없이는 `instret`이 부정확함).
- 회귀 검증(`usertests -q`/`mmaptest`) 부팅 옵션: 위와 동일하되 `-smp 3 -icount` 없음(기본 TCG) — 기능 검증 목적이라 타이밍 정확도는 불필요, 기본 QEMUOPTS(Makefile `qemu` 타깃) 그대로.
- 반복 횟수: 각 측정 조건(`trivial static/fnptr`, `RR static/fnptr`(상수 n), `RR static`(opaque n))마다 `pick_next()` 200회(`BENCH_REPS`) 연속 호출을 한 블록으로 삼아 `r_instret()` 델타/200으로 call당 평균을 냄 — calibration(§0)에서 이미 `-icount shift=0`가 완전히 결정론적임을 20/20 반복으로 검증해 두었으므로, 이 본실험은 조건당 1블록만 측정했다(다회 반복 중앙값 방식은 미적용). 실제로 CPUS=1/`-icount shift=0` 조합으로 동일 바이너리를 두 번 독립 실행한 결과가 모든 자리수까지 완전히 일치함을 재확인함(결정론 재확인, [docs/bench/dispatch_bench_icount_cpus1.txt](docs/bench/dispatch_bench_icount_cpus1.txt)).
- 원시 출력 보존: [docs/bench/dispatch_bench_icount_cpus1.txt](docs/bench/dispatch_bench_icount_cpus1.txt)(디스패치 벤치마크, CPUS=1/`-icount shift=0`), [docs/bench/usertests_raw_output.txt](docs/bench/usertests_raw_output.txt)(`usertests -q` 전체 원시 로그, CPUS=3/기본 TCG), [docs/bench/asm_run_dispatch_bench.txt](docs/bench/asm_run_dispatch_bench.txt)(`run_dispatch_bench()` 전체 역어셈블), [docs/bench/asm_fnptr_rr_pick_next.txt](docs/bench/asm_fnptr_rr_pick_next.txt)(독립 함수로 컴파일된 `fnptr_rr_pick_next` 역어셈블).
- **`usertests -q` 최종 결과: `PASS ALL TESTS`, 64개 서브테스트 전부 PASS, FAIL 0.** 로그 중간의 `usertrap(): unexpected scause 0xf ...` 다수는 xv6 스톡 `usertests`가 의도적으로 페이지 폴트를 유발해 커널 반응을 확인하는 서브테스트(`sbrkfault` 계열)의 정상 진단 출력이며 실패가 아니다(`mmaptest`에서 이미 본 것과 동일한 패턴). `mmaptest`도 별도 실행에서 `mmaptest: all tests succeeded`로 전체 통과 확인.


```c
for(p = proc; p < &proc[NPROC]; p++) {
  acquire(&p->lock);
  if(p->state == RUNNABLE) { ... swtch(...); }
  release(&p->lock);
}
```
배열을 매 틱마다 선형 스캔하는 하드코딩된 라운드로빈. 정책이 하나뿐이라 교체 불가능.

**설계 참고 (Stroustrup 2025 §4.2~4.3):** `SchedulerPolicy` concept은 논문의 `Sortable_range` 패턴을 그대로 따른다 — `requires` 절로 "정책이 제공해야 할 연산"만 명시하고(어떻게 구현하는지는 안 물음), 기본 정책을 defaulted 템플릿 인자로 두고, 정책이 요구사항을 못 채우면 인스턴스화 시점에 컴파일 에러가 나게 한다(§7.6 "Concept type matching"). 오버로딩 규칙도 논문 §4.4를 그대로 따름: 요구사항이 더 좁은 정책이 우선 선택됨.

**남은 작업 — CFS 상세 설계 (2026-08-27, `project1-braeden-hue`의 검증된 C 구현 기반, 락 반영):**

CFS는 FCFS/RR과 달리 "RUNNABLE이 되는 시점"에 트리 삽입이 필요해서, 지금의 `mark_runnable()`(FCFS 전용으로 `arrival_seq`만 찍음)을 정책-비인지형에서 **정책이 실제로 관여하는 훅**으로 일반화해야 한다. `SchedulerPolicy` concept 확장:

```cpp
// kernel/sched_policy.hpp (확장)
enum class RunnableReason { NEW, YIELD, WAKEUP };

template<typename P>
concept SchedulerPolicy = requires(P policy, struct proc* procs, int n, struct proc* p, RunnableReason r) {
    { policy.pick_next(procs, n) } -> std::same_as<struct proc*>;
    { policy.on_runnable(p, r) }   -> std::same_as<void>;   // RUNNABLE 전환마다 호출
};
```

`RunnableReason`은 C++ `enum class`라 C쪽(`proc.c`)에 그대로 노출 못 함 — `extern "C"` 경계에선 항상 `int`(0=NEW, 1=YIELD, 2=WAKEUP)로 오간다(§4에 이미 있는 "boundary는 항상 POD" 원칙과 동일선상).

```c
// kernel/proc.c -- mark_runnable을 정책-위임형으로 일반화
extern void policy_on_runnable(struct proc *p, int reason);  // kernel/scheduler.cpp

static void
mark_runnable(struct proc *p, int reason)
{
  policy_on_runnable(p, reason);
  p->state = RUNNABLE;
}
```
5개 호출 지점의 `reason` 인자: `userinit`→`RUNNABLE_NEW`, `fork`→`RUNNABLE_NEW`, `yield`→`RUNNABLE_YIELD`, `wakeup`→`RUNNABLE_WAKEUP`, `kill`→`RUNNABLE_WAKEUP`(project1의 `kkill`은 이 케이스에서 vruntime 보정을 안 했는데, 이건 원본의 누락으로 보고 우리 포팅에선 `wakeup`과 동일하게 통일한다 — 의도적 개선으로 문서에 남길 것).

```cpp
// kernel/sched_cfs.hpp
#pragma once
extern "C" { #include "types.h" #include "proc.h" }
#include "sched_policy.hpp"
#include "intrusive_tree.hpp"

constexpr bool cfs_less(struct proc* a, struct proc* b) { return a->virtualRuntime < b->virtualRuntime; }

struct CFS {
    IntrusiveTree<struct proc, cfs_less> tree;
    bool inited = false;
    void ensure_init() { if (!inited) { tree.init("cfs_tree"); inited = true; } }

    struct proc* pick_next(struct proc*, int) {
        ensure_init();
        return tree.pop_first();   // O(log N), find+remove가 트리 락 안에서 원자적
    }

    void on_runnable(struct proc* p, RunnableReason reason) {
        ensure_init();
        if (reason == RunnableReason::WAKEUP) {
            // project1-braeden-hue의 CFS 공정성 기법 그대로 이식:
            // 오래 잔 프로세스가 낮은 vruntime으로 깨어나 CPU를 독점하지 못하게,
            // 깨어날 때 vruntime을 트리의 현재 최소값 이상으로 끌어올림
            if (struct proc* min = tree.first())
                if (min->virtualRuntime > p->virtualRuntime)
                    p->virtualRuntime = min->virtualRuntime;
        }
        tree.insert(p);
    }
};
```

`kernel/proc.h`에 추가할 POD 필드(project1과 동일한 의미, 이름은 트리 재사용을 위해 일반화):
```c
int niceValue;                       // -3..2 (project1과 동일한 범위 유지)
uint64 virtualRuntime;
struct proc *left, *right, *parent;  // IntrusiveTree 노드, 임베드(§1 POD 규칙)
int color;                           // 0=RED, 1=BLACK
```
`yield()`에서 vruntime 증가(project1 그대로): `p->virtualRuntime += 1 << (p->niceValue + 3);` — 이 계산 자체는 C++로 옮길 필요 없이 `mark_runnable` 호출 직전 `proc.c`(C)에 그대로 둬도 무방(POD 필드 산술이라 언어 무관).

`kernel/scheduler.cpp` 쪽 변경: `g_policy`를 함수-로컬 `static`에서 **파일-스코프 `static`**으로 승격해야 `scheduler_dispatch()`와 `policy_on_runnable()` 양쪽에서 같은 인스턴스(같은 트리, 같은 락)를 공유한다:
```cpp
static ActivePolicy g_policy;
extern "C" [[noreturn]] void scheduler_dispatch(void) { dispatch(g_policy); }
extern "C" void policy_on_runnable(struct proc* p, int reason) {
    g_policy.on_runnable(p, static_cast<RunnableReason>(reason));
}
```
이 변경은 FCFS/RR도 같은 concept를 만족해야 하므로, 두 struct에 `on_runnable`을 추가해야 한다(FCFS는 지금 `proc.c`에 있는 `next_seq`/`seq_lock` 카운터를 자기 안으로 옮겨서 `on_runnable`에서 `arrival_seq`를 찍고, RR은 삽입 시점에 할 일이 없어 빈 `on_runnable`). **이미 검증된 FCFS/RR 코드를 건드리는 리팩터라, 이 작업 후엔 `usertests -q`/`mmaptest`를 반드시 재확인한다.**

**락 순서 (데드락 방지, 반드시 지킬 것):** `mark_runnable()`은 호출 시점에 이미 `p->lock`을 쥔 채로 `policy_on_runnable()`→`on_runnable()`→`tree.insert()`(트리 락 획득)로 이어진다 — 즉 **`p->lock`이 항상 바깥, 트리 락이 항상 안쪽**이다. 반대로 `pick_next()`(→`tree.pop_first()`)는 `dispatch()`에서 어떤 `p->lock`도 쥐지 않은 상태로 호출되고, 트리 락을 얻었다 완전히 반납한 뒤에야 `next->lock`을 새로 잡는다 — 트리 락과 `next->lock`이 절대 동시에 물리지 않는다. 이 순서가 어긋나면(트리 락을 쥔 채로 `p->lock`을 새로 잡는 경로가 하나라도 생기면) 데드락 가능성이 생기므로, CFS 구현 시 이 불변조건을 코드 리뷰 체크리스트에 넣는다.

**CFS 관련 Phase는 순서상 Phase 2(트리 템플릿 완성) 이후에 온다** — 지금 이 섹션은 설계만 확정된 상태, 구현은 Phase 2 완료 후 진행.

### Phase 1.8 — 우선순위 기반 선점: 지연 민감 작업 보호 (2026-09-12 범위 재조정)

**목표(재조정):** "모던 C++로 실행 정책과 선점 조건을 정적으로 구성하는 구조를 만들고, 지연 민감 작업을 보호하는 기능의 추가 비용을 검증한다." CFS/RBTree/2×3 조합 계획은 **보류**(설계는 위 Phase 2에 남겨둠, 의무 아님) — 팀원 피드백대로 "어떤 스케줄러가 좋은가"로 중심이 옮겨간 것을 되돌려, RR 기준선 대비 **우선순위 기반 선점 정책 하나만** 비교한다. 작업 길이와 긴급성은 별개 정보이므로, 긴급성은 burst 길이로 추론하지 않고 프로세스가 명시적으로 선언한다.

**두 작업 종류만 사용:** A(백그라운드 긴 계산, 여러 개), B(응답시간 목표가 있는 지연 민감 요청). A가 실행 중일 때 B가 도착하면 즉시 넘겨받는지가 핵심 질문.

**RTOS의 인지/판단/전환 3단계와 xv6 매핑:**
- **인지**: B가 RUNNABLE이 됐다는 걸 언제 아는가 — 이번 범위에서는 **tick 단위로만** 인지한다(기존 [kernel/trap.c:94](kernel/trap.c:94) 타이머 훅 재사용). 즉 인지 지연의 상한은 tick 주기 하나 — 이벤트 즉시 재스케줄(wakeup 시점에 바로 확인)은 범위 밖, 향후 과제로 명시.
- **판단**: `pick_next()`가 RUNNABLE 중 `Priority::LatencySensitive`를 최우선으로 고르고(동순위는 RR), 없을 때만 Normal 중 RR. `should_preempt(running)`은 **Normal이면 RR과 동일하게 매 tick 무조건 true**(중요한 정정, 아래 참고), LatencySensitive면 false(끊지 않고 끝까지 돌림).
- **전환**: xv6은 이미 안전하다 — 강제 선점은 `usertrap()`의 유저모드 복귀 지점 한 곳에서만 일어나고 커널 코드는 절대 도중에 끊기지 않는다. 새로 만들 것 없음, 구조적으로 이미 만족됨(Adios가 IPI를 따로 만들어야 했던 것과 대비되는 지점).

**새 POD 필드(`kernel/proc.h`):** `int priority;` (0=Normal, 1=LatencySensitive) — 명시적 syscall로 설정(추론 금지).

**정책 (`kernel/sched_priority.hpp`, concept은 기존 `pick_next`+`should_preempt` 그대로 재사용):**
```cpp
enum class Priority : int { Normal = 0, LatencySensitive = 1 };

template<bool UseCounter>
struct PriorityPreempt {
    int last = -1;
    struct proc* pick_next(struct proc* procs, int n) {
        // g_ls_runnable_count: O(1)로 "1차 스캔이 애초에 의미 있는지" 확인.
        // UseCounter=false는 이 단축 없이 매번 1차 스캔을 강행하는 "멍청한"
        // 대조군 -- 판단비용 비교(④)는 여기서 일어난다.
        bool maybe_ls = UseCounter ? (g_ls_runnable_count > 0) : true;
        if (maybe_ls) {
            for (int i = 1; i <= n; i++) {                   // 1순위: LatencySensitive
                int idx = (last + i) % n; struct proc* p = &procs[idx];
                if (p->state == RUNNABLE && p->priority == (int)Priority::LatencySensitive)
                    { last = idx; return p; }
            }
        }
        for (int i = 1; i <= n; i++) {                        // 2순위: Normal, RR
            int idx = (last + i) % n; struct proc* p = &procs[idx];
            if (p->state == RUNNABLE) { last = idx; return p; }
        }
        return nullptr;
    }
    // Normal은 RR과 완전히 동일하게 매 tick 무조건 양보 -- 우선순위 메커니즘의
    // 효과는 전부 위 pick_next()의 재정렬에서 나오고, should_preempt()는
    // Normal끼리의 기본 공정성(타임슬라이싱)을 절대 건드리지 않는다.
    // LatencySensitive는 끊지 않고 끝까지 돌림(규칙 1).
    bool should_preempt(struct proc* running) {
        return running->priority != (int)Priority::LatencySensitive;
    }
};
```

**⚠️ Step 2에서 발견·수정한 실제 설계 버그:** 최초 구현은 `should_preempt()`가 Normal에 대해 `g_ls_runnable_count > 0`을 확인했다 — "LatencySensitive가 없으면 양보 안 함"이라는 뜻인데, 아직 아무도 `priority=1`을 쓰지 않는 상태(syscall 미구현)에서는 이 카운터가 항상 0이라 **Normal 프로세스가 영원히 tick 양보를 안 하게 되는** 실제 회귀였다. `usertests -q`의 `test preempt`(동일 우선순위 CPU-bound 자식들의 타임슬라이싱을 검증)가 정확히 이 지점에서 멈춰 잡아냈다. 교훈: "지연 민감 작업을 보호할 이유가 없다"와 "선점할 이유가 아예 없다"를 혼동하면 안 된다 — 같은 우선순위끼리의 기본 공정성은 우선순위 메커니즘과 무관하게 항상 보장돼야 한다. 수정 후 Normal의 `should_preempt()`는 RR과 완전히 동일(무조건 true)해졌고, O(1)/O(N) 판단비용 비교는 `should_preempt()`가 아니라 `pick_next()`의 1차 스캔 단축 여부로 옮겼다 — 이 위치가 실제로 의미 있는 차이를 만드는 유일한 지점이기 때문(Step 2 재검증: `usertests -q` PASS ALL TESTS, `mmaptest` all tests succeeded).

**측정 지표 (핵심 결과, `instret/call`에서 여기로 이동):**
1. B의 응답시간 분포(P50/P99) **+ 목표 응답시간 초과 비율**(단일 P99보다 RTOS 관점에 맞음)
2. A의 진행 — 완료시간(끝난다면) 및 고정 구간 내 실제 받은 CPU tick 수(굶는지 관찰 — **RR과 달리 이 정책은 A의 실행 기회를 구조적으로 보장하지 않는다, 의도적 관찰 대상**)
3. 추가 판단비용 + 문맥전환 횟수 — `UseCounter=true/false` 두 빌드를 오늘 확립한 `-icount shift=0` + `r_instret()` 기법으로 비교(오늘 실험의 직접 연장선)

**C++ 요소:** concept(정책 인터페이스 검사), `if constexpr`(기능 on/off에 따라 관련 코드를 컴파일 단계에서 제외 — 오늘 `nm`으로 이미 증명한 "안 쓰는 정책은 코드생성 자체가 없다"의 연장), 단위 혼동 방지용 강타입(tick 기반 응답시간 값과 이번 세션에서 다뤘던 cycle/instret 값을 타입으로 구분). **다만 concept이 선점 안전성이나 데드라인 달성 자체를 증명해주진 않는다 — 연구 성공 기준은 문법 사용 여부가 아니라 실제 대기 감소 여부.**

**한계(미리 명시):** hard real-time 보장을 주장하지 않는다 — "긴 CPU 작업이 섞일 때 지연 민감 작업의 목표 시간 초과율을 줄이는가"까지만. 인지 단계를 tick 단위로 좁혔으므로 tick 주기 자체가 응답시간의 하한으로 섞여 들어간다(결과 해석 시 분리해서 보고). A의 starvation 가능성은 우선순위 역전 방지(aging 등) 없이 그대로 관찰한다 — 필요하면 이후 과제.

**구현 순서(각 단계 `usertests -q`/`mmaptest` 회귀 확인):**
1. ✅ `priority` 필드(`kernel/proc.h`) + concept 확장(`should_preempt`) + trap.c 훅([kernel/trap.c:94](kernel/trap.c:94), usertrap()만). RR은 `should_preempt` 항상 true라 행동 변화 0인 순수 리팩터 — `usertests -q` PASS ALL TESTS, `mmaptest` all tests succeeded로 확인.
2. ✅ `PriorityPreempt<true>` 추가(`g_ls_runnable_count`는 `mark_runnable()`/`dispatch()`에 직접 증감 — `RunnableReason` 일반화는 필요 없어서 안 씀), `ActivePolicy`로 전환해 빌드/부팅 확인. **버그 1건 발견·수정**(위 참고) 후 재검증 완료.
3. ✅ `user/latencytest.c`(A 4개 백그라운드 + B 20라운드, `setpriority` syscall 신규 추가) 작성, RR vs `PriorityPreempt<true>` 측정 완료 — 결과는 아래.
4. `PriorityPreempt<false>`(O(N))로 판단비용만 재측정, 문맥전환 횟수 계측 추가.

**⚠️ Step 3 최초 측정 (아래 v2로 대체됨 — `wait()` 오회수 버그로 15~20라운드 데이터 오염, 원인은 바로 아래 결함 1번 참고):**

| | RR (기준선) | `PriorityPreempt<true>` |
|---|---|---|
| B 응답시간(tick, 정렬) | `0 0 0 0 3 3 4 4 4 4 4 4 9 12 13 19 19 19 19 24` | `7 7 7 7 7 7 7 7 7 7 7 7 7 7 7 7 7 7 7 8` |
| p50 / p99 | 4 / **24** | **7** / 8 |
| 목표(15tick) 초과율 | **5/20 (25%)** | **0/20 (0%)** |
| A 4개의 결과(같은 테스트 구간 내) | **전부 완료**(tick 138/138/138/152) | **전부 미완료**(tick 167에 강제 종료, 진행 중이던 채로 killed) |

**해석:** `PriorityPreempt<true>`는 B의 응답시간을 극적으로 예측 가능하게 만든다(표준편차 사실상 0, p99가 RR의 1/3) — RR은 최선의 경우(0~4tick)엔 오히려 더 빠를 수 있지만(B가 막 스스로 선언하기 전에 운 좋게 빈 틈을 만나는 경우), 최악의 경우(24tick, 목표의 1.6배)엔 훨씬 나쁘다. 이게 Adios 논문이 말하는 HOL blocking의 실측 재현이다. **그 대가는 명확하다: 같은 시간 동안 A가 전혀 진행하지 못했다** — B가 스스로를 선언한 뒤엔 `should_preempt()`가 매번 false를 반환해 끊기지 않고 끝까지 도는데, 그 몇 tick 동안 A는 완전히 배제된다. 20라운드 동안 이게 누적되며 A 4개 전부가 굶었다. **이것이 정확히 "긴 작업에도 실행 기회를 보장한다"는 원래 규칙과 충돌하는 지점**이며, plan.md에 미리 명시했던 관찰 대상(starvation)이 실제로 관측된 사례다. RR 대비 "어떤 조건에서 유리하고 어떤 조건에서 비용만 느는지"에 대한 1차 답: **B가 드물고 짧다면 이 방식이 이득이지만, B가 자주/많이 발생하면 A는 사실상 굶는다** — 이 임계점(B의 빈도·길이 대비 A 기아 정도)을 정량화하는 게 다음 실험 후보.

**측정 한계(정직하게 명시):** B의 최초 디스패치(4tick 대기)는 두 정책 모두 동일 — RR-폴백 경쟁 구간이라 우선순위 메커니즘이 아직 개입 못 함(코드 주석에 이미 명시한 부트스트랩 한계, 실측으로 확인됨). CPUS=1(단일 코어)에서만 측정 — 멀티코어에서는 A들이 다른 코어로 분산돼 경쟁 자체가 줄어들 것이므로 결과가 달라질 수 있음, 별도 측정 필요. 문맥전환 횟수는 아직 계측 안 함(4단계에서 추가 예정).

**⚠️ 외부 검토(2026-09-12)로 발견한 실제 결함 — 다음 실험 전 수정 필요:**
1. ✅ **수정됨 — `wait(0)` 오회수 버그:** [kernel/proc.c:449](kernel/proc.c:449)의 `kwait()`는 `proc[]`을 스캔해 **처음 찾은 ZOMBIE 자식**을 회수한다(호출자가 기다리는 pid와 무관). `latencytest.c`의 부모는 A 4개 + 매 라운드 B를 동시에 자식으로 두므로, A가 종료하는 순간 B용 `wait(0)`가 A를 대신 회수할 수 있었다. `wait_for(target)` 헬퍼(반환 pid가 기대한 자식이 아니면 재시도)로 [user/latencytest.c](user/latencytest.c)에서 수정, v2 재측정으로 확인.
2. **미수정 — `should_preempt()`에 상한이 없음:** [kernel/sched_priority.hpp:66-67](kernel/sched_priority.hpp:66)은 `running->priority == LatencySensitive`만 보고 **얼마나 오래 실행했는지는 안 본다** — "지연 민감"이 "짧다"를 보장하지 않는다는 뜻. 자진 선언한 프로세스가 실제로 길게 돌면 Normal뿐 아니라 **다른 LatencySensitive까지 무기한 굶을 수 있다.** 지금 워크로드(B가 한 번에 하나, 짧게)에서는 안 드러났을 뿐, 설계상 진짜 취약점 — budget 축 도입 시 함께 해결.
3. ✅ **수정됨 — P99 표본 수 부족:** `NSHORT`를 20→100으로 올려 `(NSHORT*99)/100=99`가 실제 백분위수가 되도록 함(v2).
4. ✅ **수정됨 — A를 강제 종료시켜 처리량/공정성 미측정:** `kill()` 제거, A가 끝까지 자연 완료하도록 바꾸고 완료 tick을 직접 비교(v2 결과 참고). 다만 "작업별 CPU 지분"(순간순간의 배분 비율)까지는 아직 안 잼 — 완료시간이라는 총량 지표만 있음.
5. **미수정, 의도적 범위 밖 — kerneltrap()은 여전히 무조건 `yield()`**([kernel/trap.c:169](kernel/trap.c:169)) — 정책이 전체 실행 경로에 일관되게 적용된 건 아니라는 점을 결과 해석 시 명시해야 한다.

**연구 질문 재정의 (다음 설계 가설, 아직 미구현):** "작업의 지연 요구를 우선순위에 반영하되, CPU 사용 예산과 대기 시간 보정을 어떻게 결합해야 tail latency를 낮추면서 다른 작업의 진행을 보장할 수 있을까?" 세 축을 구분해서 설계한다 — **우선순위**(얼마나 급한가, 명시적 선언), **연속 실행 quantum**(한 번 선택됐을 때 얼마나 오래 도는가), **누적 CPU budget**(일정 기간 얼마나 쓸 수 있는가, 소진 시 강등·보충 규칙 포함). quantum만 제한하면 strict priority 선택 때문에 Normal이 계속 밀릴 수 있으므로, 가중 CPU 지분/budget 소진 시 강등/aging 중 무엇으로 최소 진행을 보장할지가 검증 대상이다.

**✅ Step 3 v2 측정 결과 (2026-09-12, 결함 1~2·4 수정 후 재측정 — `wait_for()`로 오회수 방지, `NSHORT=100`, A는 끝까지 자연 완료시킴. 워크로드는 v1과 동일: A 4개 각 6억 회, B 각 1억 회, `DEADLINE_TICKS=15`):**

| | RR (기준선) | `PriorityPreempt<true>` |
|---|---|---|
| B p50 / p99 | 4 / 19 | 4 / **8** |
| 목표(15tick) 초과율 | 5/100 (5%) | **0/100 (0%)** |
| A 4개 완료 tick(전부 자연 완료) | **132, 132, 132, 141** | **184, 185, 193, 290** |
| A 완료까지 걸린 시간(시작 tick 대비) | ≈115tick | ≈**263tick (약 2.3배)** |
| 전체 테스트 길이 | tick 26→488 (462) | tick 27→483 (456) — 비슷함 |

**정정된 해석:** 오회수 버그를 없애자 v1의 "A 전부 미완료"라는 극단적 결과는 사라졌다 — `PriorityPreempt`에서도 A는 결국 다 끝난다(무한 굶주림은 아님). 대신 **정량화된 트레이드오프**로 바뀌었다: B의 tail latency(p99 19→8, 절반 이하)와 deadline 초과율(5%→0%)은 확실히 개선되지만, 그 대가로 **A의 완료 시간이 약 2.3배 늘어난다**(115→263tick). p50은 두 정책이 거의 같다(4 vs 4) — 차이는 tail(p99)과 A의 처리량에서만 나타난다. 이게 v1보다 훨씬 정직하고 방어 가능한 결과다: "B가 가끔 온다면 A가 결국 끝나면서도 B의 꼬리 지연을 절반으로 줄일 수 있다"는, 실제로 쓸모 있는 절충으로 표현할 수 있다.

**다음 실제 작업 순서:**
1. ✅ `wait()` 오회수 수정, `NSHORT=100`, A 자연 완료 — 완료, 결과는 위 v2.
2. `CPUS=1`에서 **RR → strict priority(현재 `PriorityPreempt`) → budget 추가 priority**(위 세 축 중 budget까지 구현한 새 정책) 순서로 비교.
3. 요청 도착률·burst·작업 길이 편차를 바꿔가며 재측정.
4. 배경 작업의 단위시간당 완료량·작업별 CPU 지분·최대 runnable 대기시간까지 계측 추가(현재는 A의 총 완료시간만 있음, 세분화된 CPU 지분은 아직 없음).

**교수님께 설명할 때 표현:** "C++로 대기 시간을 줄였다"가 아니라 **"정책 비교 기반을 구현했고, 이를 통해 응답 시간·처리량·공정성의 절충을 연구하려 한다"**가 지금 코드 상태와 정확히 맞는다. 디스패치 실험도 "정적 디스패치가 빠르다"가 아니라 **"간접 호출 비용과 컴파일러 최적화 효과를 분리해 분석했다"**로 표현하는 게 정확하다(§Phase 1의 GCC 강도감소 발견 참고).

**ADIOS 연결점 재정정:** ADIOS는 "busy-wait/yield를 선택하는 정책" 연구가 아니라, 페이지 폴트 핸들러와 스케줄러를 같은 주소공간에 두고 가벼운 unikernel 스레드로 **양보 비용 자체를 낮춰 스케줄링의 유불리를 바꾼** 연구다. 이 프로젝트와 맞닿는 지점은 "언제 CPU를 넘길지"와 "넘기는 비용이 전체 지연에 주는 영향"이며, xv6의 tick 단위 실험을 ADIOS의 마이크로초 RDMA 환경에 그대로 일반화할 수는 없다 — 연구 관심의 확장으로 설명한다.

### Phase 1.5 — mmap 타입 안전성 리팩터 (Phase 2~5와 독립, 언제 해도 무방)

**✅ 구현 완료 (2026-08-25):** [kernel/sysfile_mmap.cpp](kernel/sysfile_mmap.cpp)로 실제 이식됨. `sys_mmap`을 [kernel/sysfile.c](kernel/sysfile.c)에서 삭제하고, `argfd`를 `static` 해제([kernel/defs.h](kernel/defs.h)에 프로토타입 추가)해서 새 `.cpp` 파일에서 재사용. `Makefile`에 `$K/%.o: $K/%.cpp` 규칙과 `sysfile_mmap.o`를 `OBJS`에 추가.

빌드 중 컴파일러가 실제로 버그를 하나 잡아냈다 — `.length = length` (int → 구조체의 `uint64` 필드)가 `error: narrowing conversion` 로 거부됨. C의 평범한 대입(`vma->length = length;`)은 이 축소 변환을 조용히 허용하지만, C++20 designated initializer는 brace-init 규칙상 축소 변환을 금지한다. `(uint64)length`로 명시적 캐스팅해서 해결 — **이게 §6에서 주장한 "정적 타입 안전성"의 실측 사례**다(계획 단계에서 예상 못 했던 보너스 발견).

전체 커널을 clean rebuild로 링크까지 검증했고(`nm`으로 `sys_mmap` 심볼이 mangling 없이 정상 노출됨을 확인), QEMU에서 `mmaptest`를 실제로 돌려 전체 서브테스트(`basic mmap`, `private`, `read-only`, `read/write`, `dirty`, `not-mapped unmap`, `lazy access`, `two files`, `fork`, `munmap prevents access`, `writes to read-only`)가 `mmaptest: all tests succeeded`로 통과함을 확인 — 기능적으로 원본 C 버전과 동일하게 동작.

**주의:** §0에서 정한 "속도 증명" 명제와 무관한 **별개의 명제**다 — mmap은 여전히 정적 디스패치 비교의 대상이 아니다. 여기서 증명하는 건 **정적 타입 안전성**이다. 측정 방식도 rdcycle/objdump가 아니라 **"어떤 버그 클래스가 런타임 실수에서 컴파일 에러로 격상됐는가"**로 한다.

**Before ([kernel/sysfile.c](kernel/sysfile.c) `sys_mmap`, 원본):**
```c
uint64
sys_mmap(void)
{
  int length,prot,flags,fd,offset;
  struct file *f;
  struct proc *p;
  struct vma *vma;        // <- 지역변수 이름이 타입명(vma)과 동일
  uint64 addr;
  ...
  addr = TRAPFRAME - PGROUNDUP(length);   // 매크로, 타입 무검사
  for(int j = 0; j < NVMA; j++){
    ...
    if (/* 충돌 */) { addr = p->vmas[j].addr - PGROUNDUP(length); j=-1; continue; }
  }
  vma->used = 1;           // 7줄에 걸친 필드별 대입, 순서 실수를 컴파일러가 못 잡음
  vma->addr = addr;
  vma->length = length;
  vma->prot = prot;
  vma->flags = flags;
  vma->offset = offset;
  vma->f = filedup(f);
  return addr;
}
```

**After ([kernel/sysfile_mmap.cpp](kernel/sysfile_mmap.cpp), 실제 구현):**
```cpp
constexpr uint64 page_round_up(uint64 sz) {
    return (sz + PGSIZE - 1) & ~(PGSIZE - 1);
}

extern "C" uint64 sys_mmap(void)
{
    int length, prot, flags, fd, offset;
    struct file *f;
    struct proc *p = myproc();

    argint(0, &length);
    argint(1, &prot);
    argint(2, &flags);
    if (argfd(3, &fd, &f) < 0) { return -1; }
    argint(4, &offset);
    if (length <= 0 || fd < 0) { return -1; }

    struct vma *free_vma = nullptr;
    for (auto &v : p->vmas) {
        if (!v.used) { free_vma = &v; break; }
    }
    if (!free_vma) { return -1; }

    const uint64 aligned_len = page_round_up(length);
    uint64 addr = TRAPFRAME - aligned_len;

    bool collision = true;
    while (collision) {
        collision = false;
        for (const auto &v : p->vmas) {
            if (!v.used) continue;
            if (addr < (v.addr + v.length) && (addr + aligned_len > v.addr)) {
                addr = v.addr - aligned_len;
                collision = true;
                break;
            }
        }
    }

    *free_vma = vma{
        .used = 1,
        .addr = addr,
        .length = (uint64)length,   // 축소 변환 명시적 캐스팅 (컴파일러가 잡아낸 지점)
        .prot = prot,
        .flags = flags,
        .offset = offset,
        .f = filedup(f)
    };
    return addr;
}
```

**무엇이 바뀌었는가 (검증된 것만):**

| C 스타일 | → | C++ 대체 | 실측 결과 |
|---|---|---|---|
| `PGROUNDUP(length)` 매크로 | → | `constexpr uint64 page_round_up(uint64)` | 타입 안전 인라인 함수로 교체됨. 단, `length`가 syscall 런타임 인자라 `constexpr`이 여기서 **컴파일타임 평가를 일으키진 않는다** — 이득은 "컴파일타임 계산"이 아니라 타입 무검사 매크로 치환 제거 |
| 필드별 순차 대입 7줄 | → | designated initializer | **실측 확인**: 선언 순서와 다르게 쓰면 `error: designator order for field 'vma::prot' does not match declaration order` — 순서 실수가 하드 컴파일 에러로 격상됨. 단, 원본이 지역변수를 `struct vma *vma`로 타입명과 동일하게 지어서, 이름을 바꾸지 않고 그대로 `vma{...}`를 쓰면 타입명을 가려(shadow) 컴파일이 안 됨 — `free_vma`로 개명이 스타일이 아니라 **필수 조치**였음을 실측으로 확인 |
| `j=-1; continue;`로 배열 처음부터 재시작 | → | `while(collision)` 명시적 상태 플래그 | 복잡도(O(N²))는 동일, **가독성만 개선** — 과장하지 않음 |
| 인덱스 `p->vmas[j]` | → | `auto& v : p->vmas` (range-based for) | 실질 효과는 "매 접근마다 바운드 체크"가 아니라 **루프 경계를 사람이 손으로 안 써도 되게 만드는 것** — 이번 코드에서 새로 막는 버그는 없지만(원본 `j<NVMA`가 이미 정확) 향후 경계 오타를 원천 차단 |

### Phase 1.6 — 자원/에러 안전성: SerenityOS AK 패턴 도입 (`RefPtr`, `ErrorOr`/`TRY`)

**배경:** Phase 1.5에서 "자원 안전성 미완(gap)"으로 명시적으로 남겨뒀던 `vma->f` raw pointer 문제를 실제로 해결하려던 단계. SerenityOS의 AK 라이브러리는 xv6과 동일한 제약(freestanding, 예외/RTTI 없음)에서 이미 실제로 부팅되는 커널로 검증된 두 패턴을 쓴다 — xv6엔 이미 `struct file`에 수동 참조카운트(`filedup`/`fileclose`, [kernel/defs.h:30-31](kernel/defs.h:30))가 있으므로 참조카운트 로직 자체를 새로 만들 필요는 없다는 아이디어였다.

**실제 사례 (원 제출 리포트 `OS_project3_HYU-ELE3021_2019069270.pdf`에서 확인, ErrorOr가 왜 필요한지의 살아있는 근거):** 원본 C 구현 당시 `mmapfault()`가 성공 시 무엇을 리턴해야 하는지 자체 컨벤션이 명확하지 않아서, 처음엔 성공해도 [kernel/trap.c](kernel/trap.c)의 `mmapfault(stval)==0` 체크와 안 맞는 값을 리턴해 **매핑까지 성공한 페이지가 매번 "실패"로 오인되어 프로세스가 죽는 버그**가 실제로 발생했다(`usertrap(): unexpected scause`로 나타났다가, 리턴 컨벤션을 성공=0으로 통일하고서야 해결). 이건 전형적인 "정수 하나로 성공/실패/값을 다 표현하려다 컨벤션이 꼬이는" C 에러처리 문제다 — `ErrorOr<T>`를 썼다면 "성공(값 O)"과 "실패(에러 O)"가 애초에 타입으로 분리돼 있어서 이런 컨벤션 불일치 자체가 성립할 수 없다. (다만 이 버그는 이미 원본 코드에서 해결되어 있고 Phase 1.5/1.6은 `mmapfault`를 건드리지 않으므로, 이 사례는 "왜 ErrorOr가 유용한 패턴인가"를 보여주는 근거로만 인용하고 실제 코드 변경 대상은 아니다.)

**❌ 1. `RefPtr<T, IncRef, DecRef>`을 `struct vma.f`에 임베드 — 재검토 후 기각 (2026-08-25)**

원래 계획은 아래와 같았다:
```cpp
// kernel/ref_ptr.hpp
template<typename T, T*(*IncRef)(T*), void(*DecRef)(T*)>
class RefPtr {
    T* ptr = nullptr;
public:
    RefPtr() = default;
    explicit RefPtr(T* p) : ptr(p) {}
    RefPtr(const RefPtr& o) : ptr(o.ptr ? IncRef(o.ptr) : nullptr) {}
    RefPtr(RefPtr&& o) noexcept : ptr(o.ptr) { o.ptr = nullptr; }
    ~RefPtr() { if (ptr) DecRef(ptr); }
    RefPtr& operator=(RefPtr o) noexcept { T* t=ptr; ptr=o.ptr; o.ptr=t; return *this; }
    T* operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};
using FileRef = RefPtr<file, filedup, fileclose>;
// struct vma의 `struct file *f;` -> `FileRef f;`로 교체할 계획이었음
```

**기각 사유 — 실제 코드 추적으로 확인한 두 가지 문제:**

1. **아키텍처 블로커**: `struct vma`는 [kernel/proc.h](kernel/proc.h)에서 `struct proc` 안에 **값으로** 임베드돼 있다(`struct vma vmas[NVMA];`). `proc.h`는 `trap.c`/`sysproc.c`/`exec.c`/`file.c`/`pipe.c` 등 커널 거의 전체가 include한다. `vma.f`를 생성자/소멸자 있는 `RefPtr`로 바꾸면 그 순간 `struct proc`도 "C++ 전용 타입"이 되어, `proc.h`를 include하는 모든 `.c` 파일이 **파싱 자체가 안 된다**(타입 불일치가 아니라 문법 에러). 4개 함수만 포팅하는 걸로는 안 되고 커널 전체를 C++로 포팅해야 하는 규모라 비현실적.
2. **완화안(로컬 스코프 한정)도 무의미함이 밝혀짐**: "구조체 필드는 raw pointer로 두고 각 C++ 함수 안에서만 지역변수로 `FileRef`를 쓰자"는 완화안을 검토했으나, `mmapfault`/`sys_munmap`/`kfork`의 실제 제어 흐름을 다시 추적해보니 **애초에 `filedup`↔`fileclose` 사이에 early return이 끼는 함수가 하나도 없다**(mmapfault는 filedup/fileclose를 아예 안 부르고, sys_munmap/kfork는 각각 무조건 끝까지 실행되는 단일 경로). 즉 Phase 1.6이 막으려던 진짜 위험(4곳에 흩어진 걸 나중에 헷갈리는 **함수 간** 컨벤션 문제)은 로컬 스코프 RAII로 전혀 해결되지 않는다 — 겉보기엔 RAII를 쓰지만 실질적 안전성 이득이 없는 코드가 된다.

**결론: `RefPtr`-in-`struct vma`는 채택하지 않는다.** 일반 규칙은 §1에 기록.

**2. `ErrorOr<T>`/`TRY()` (이 Phase의 실제 산출물 — `RefPtr` 기각과 무관하게 유효함)**

**Before ([kernel/sysfile.c](kernel/sysfile.c) `sys_mmap`, `argint`는 [kernel/defs.h:132](kernel/defs.h:132) 확인상 실패할 수 없는 `void` 함수, `argfd`만 `int` 반환):**
```c
argint(0, &length);
argint(1, &prot);
argint(2, &flags);
if (argfd(3, &fd, &f) < 0) {return -1;}
argint(4, &offset);
```
**After:**
```cpp
struct Error { const char* msg; };
template<typename T> class ErrorOr { /* union{T,Error} + bool, 예외 없이 태그드 유니온으로 구현 */ };
#define TRY(expr) ({ auto _t = (expr); if (_t.is_error()) return -1; _t.release_value(); })

ErrorOr<FileRef> arg_file(int n) {
    int fd; struct file* f;
    if (argfd(n, &fd, &f) < 0) return Error{"bad fd"};
    return FileRef(filedup(f));
}
...
auto length = argint_val(0);
auto f      = TRY(arg_file(3));   // FileRef 획득과 동시에 RAII 편입 — 이후 어떤 에러 경로에서 return해도 자동 fileclose
```

**무엇이 바뀌었는가 (정직하게, 과장 없이):**

| 항목 | 주장 | 근거 |
|---|---|---|
| `RefPtr`(`struct vma` 필드로) | ❌ 기각됨 (위 사유 참고) | — |
| `FileRef`(=`RefPtr<file,...>`)를 **지역변수로만** 사용 | `arg_file()`처럼 함수 하나의 스코프 안에서 값을 주고받을 때는 여전히 유효 — 구조체 필드가 아니라 지역변수/리턴값이라 C 쪽엔 전혀 안 보임 | 실측: 컴파일 클린, `filedup`/`fileclose` 로직 자체는 변경 없음(재사용) |
| `ErrorOr`/`TRY` | **기존 버그를 고치는 게 아니라 미래를 방지** — 현재 `argint`는 실패할 수 없어서(void 반환) 지금 당장 leak 버그는 없음. 다만 향후 에러 경로가 추가될 때 `FileRef`가 이미 RAII로 편입돼 있으면 해당 경로에서 별도로 `fileclose`를 안 챙겨도 자동으로 정리됨 | `TRY()`는 GCC/Clang의 statement expression 확장(`({ ... })`)에 의존 — ISO C++ 표준은 아니지만 이 프로젝트는 이미 gcc/g++ 전용 툴체인이라 문제 없음. 이 의존성은 §1에 기록 |

**Contemporary C++ 특성 매핑:**
- ⚠️ **자원 안전성 (부분적, 정직하게 축소)**: Phase 1.5의 gap(`vma->f` 수동 관리)은 **구조체 차원에서는 해소되지 않았다** — `RefPtr`-in-`struct vma`가 기각됐기 때문. `FileRef`를 함수 지역변수로 쓰는 한도 내에서만 자원 안전성 이득이 있음(제한적 증명)
- ✅ **정적 타입 안전성**: `ErrorOr<T>`가 "값 또는 에러"를 타입으로 강제 — 에러 체크를 깜빡하면 `release_value()`를 호출할 방법이 없어 컴파일 단계에서 어색해짐(런타임 누락이 아니라 타입 사용 불편으로 드러남)
- 해당 없음: 하드웨어 직접 제어(이 Phase는 CSR/AMO와 무관), 효율성(속도 주장 안 함, §0에서 명시적으로 배제), 안정성(단일 서브시스템 리팩터)

**미착수:** 실제 `kernel/error_or.hpp`/`kernel/ref_ptr.hpp` 코드는 스크래치패드에서 검증만 됐고 아직 실제 커널 파일로 옮기지 않음.

### Phase 1.7 — 전역 narrowing 방지: Stroustrup `Number<T>` 패턴 (1차 문헌 기반, Phase 1.6과 결합)

**출처:** Bjarne Stroustrup, *"Concept-Based Generic Programming in C++"* (Columbia University, 2025), §2 "Arithmetic conversions". 이 프로젝트의 C++ 채택 근거를 1차 문헌으로 직접 인용하는 유일한 Phase — §7.2에서 저자 본인이 "GP는 정적 다형성으로 인라인을 가능케 하고, OOP는 종종 vtable 간접호출·힙 할당·캐시 악화를 수반한다"고 명시한 대목은 Phase 1 전체 명제의 1차 근거이기도 하다.

**배경 — Phase 1.5의 catch는 "설계"가 아니라 "우연"이었다:** Phase 1.5에서 `.length = length`(int→uint64) narrowing을 designated initializer의 brace-init 규칙 덕에 잡았지만, 이건 마침 그 자리가 designated init이어서 잡힌 것뿐이다. 일반 대입문(`vma.length = length;`)이었다면 안 잡혔을 것이다. 이 프로젝트엔 narrowing을 **체계적으로** 막는 장치가 없다 — Stroustrup의 논문이 정확히 이 문제의 표준 해법을 제시한다.

**Before (일반화):** xv6 syscall 인자 파싱은 `int`/`uint`/`uint64`를 아무 검사 없이 오간다 — `argint`가 항상 `int`로 받고, 그 값이 나중에 `uint64` 필드에 대입되는 지점이 커널 곳곳에 있다(Phase 1.5의 mmap이 우연히 걸린 한 사례일 뿐).

**After (논문 §2를 이 프로젝트의 제약에 맞게 이식):**
```cpp
template<typename T>
concept Num = std::integral<T> || std::floating_point<T>;

template<typename T, typename U>
concept Can_narrow_to =
    !std::same_as<T, U> && Num<T> && Num<U>
    && ( (std::floating_point<T> && std::integral<U>)
      || (std::numeric_limits<T>::digits > std::numeric_limits<U>::digits)
      || (std::signed_integral<T> != std::signed_integral<U> && sizeof(T) == sizeof(U)) );

template<Num U, Num T>
constexpr bool will_narrow(T t) {
    if constexpr (!Can_narrow_to<T, U>) return false;   // 컴파일타임에 이미 안전 확정 — 런타임 비용 0
    // ... 논문과 동일한 부호/범위 검사 ...
}

// ⚠️ 논문 원본은 narrowing 시 `throw Bad_value{}` — 이 프로젝트는 -fno-exceptions라 그대로 못 씀.
// 대신 Phase 1.6의 ErrorOr<T>로 대체한다 — 두 Phase가 여기서 결합됨.
template<Num T>
constexpr ErrorOr<T> convert_to(Num auto u) {
    if (will_narrow<T>(u)) return Error{"narrowing"};
    return T(u);
}
```

**무엇이 바뀌었는가:**

| 항목 | 주장 | 근거 |
|---|---|---|
| 검사 비용 | `if constexpr`로 "narrowing이 애초에 불가능한 조합"(같은 타입, 더 큰 타입으로의 변환 등)은 **컴파일타임에 완전히 제거** — 런타임 검사는 진짜 narrowing 가능성이 있는 조합에서만 발생 | 논문 §2.1 인용: "First, we test what can be done at compile-time... Only if a change of value is possible do we use a run-time test" — 우리 프로젝트의 "과장 금지, 정확한 인과관계" 원칙과 정확히 부합 |
| 예외 → `ErrorOr` 대체 | 원본 논문의 `throw`는 이 프로젝트에서 못 씀(`-fno-exceptions`) — Phase 1.6에서 이미 검증한 `ErrorOr<T>`로 대체 | 실측 필요: 이 조합을 실제로 컴파일 검증하지 않았음 — 착수 시 가장 먼저 확인할 항목으로 표시 |

**Contemporary C++ 특성 매핑:**
- ✅ **정적 타입 안전성**: 이 Phase 자체가 §6 표의 "정적 타입 안전성" 항목의 1차 문헌 근거
- ✅ **효율성**: `if constexpr`로 안전이 보장된 조합은 검사 자체가 컴파일타임에 사라짐(zero-overhead) — Phase 0/1의 constexpr 상수폴딩 계열과 같은 메커니즘
- 해당 없음: 하드웨어 직접 제어, 자원 안전성(이 Phase는 값 변환만 다룸), 안정성(단일 유틸리티 추가)

**의존성:** Phase 1.6(`ErrorOr<T>`)이 먼저 있어야 `throw`를 대체할 수 있음. Phase 1.5처럼 벤치마크 인프라(Phase 0) 없이 착수 가능.

### Phase 2 — CFS용 인트루시브 템플릿 트리 (⏸️ 보류, 설계 보존)

**출처: `project1-braeden-hue`(HYU-ELE3021 project1, 같은 사용자의 이전 과제)에 이미 완성된 nice-value 기반 CFS를 실제로 발견해서 그대로 포팅 대상으로 삼는다.** 순수 C로 짠 CLRS 스타일 레드블랙 트리(`kernel/rbtree.c`/`rbtree.h`, insert/delete fixup까지 정확히 구현됨)와 `struct proc`에 임베드된 `virtualRuntime`/`niceValue`/`runNode`가 이미 있다. **실제로 찾은 버그**: 이 트리가 전역 변수(`struct rb_tree runnableTree;`)인데 이 프로젝트는 `CPUS=3`(SMP)으로 도는데도 트리 자체를 보호하는 락이 없다 — 개별 `p->lock`만 잡고 `rb_insert`/`rb_delete`/`rb_first`를 호출하므로, 서로 다른 코어가 동시에 회전(rotation)하면 `root`/`parent` 포인터가 꼬일 수 있는 실제 SMP 레이스 컨디션이다. 우리 포팅에서는 **전용 스핀락을 트리 자체에 내장**해서 고친다(Phase 1의 `seq_lock` 패턴과 동일).

**Before (C, 범용 BST를 짠다면):** `void*` + 함수포인터 비교자를 쓰는 범용 BST가 되어, 노드 비교마다 간접호출 발생. (실제로 `project1-braeden-hue`의 버전은 `void* data`로 뒤로 참조하는 "느슨한 인트루시브"였다 — `key`(uint64) 필드와 `data`(역참조용 void*)를 별도로 뒀다.)

**After — `kernel/intrusive_tree.hpp` (project1의 rbtree.c를 그대로 템플릿화 + 락 추가, 알고리즘 로직 자체는 변경 없음):**
```cpp
#pragma once
extern "C" {
#include "types.h"
#include "spinlock.h"
}

enum class RbColor : int { RED = 0, BLACK = 1 };

// T는 다음을 평범한 POD 필드로 직접 갖고 있어야 한다 (§1 POD-only 규칙):
//   T *left, *right, *parent;   int color;   // 0=RED, 1=BLACK
// Compare(a, b) -> bool : "a가 b보다 먼저 오는가" (strict weak order)
template<typename T, auto Compare>
class IntrusiveTree {
    T* root_ = nullptr;
    struct spinlock lock_;   // project1엔 없던 것 -- SMP 레이스 수정

    static RbColor col(T* n)             { return n ? static_cast<RbColor>(n->color) : RbColor::BLACK; }
    static void set_col(T* n, RbColor c) { if (n) n->color = static_cast<int>(c); }

    // --- 아래 rotate/fixup/transplant는 project1-braeden-hue/kernel/rbtree.c의
    //     _left_rotate/_right_rotate/_insert_fixup/_rb_transplant/_delete_fixup을
    //     기계적으로 옮긴 것 -- key 비교 대신 Compare(), void* 대신 T*, RED/BLACK
    //     enum 대신 col()/set_col() 헬퍼만 바뀌었고 알고리즘 자체는 동일 ---
    void left_rotate(T* x) {
        T* y = x->right;
        x->right = y->left;
        if (y->left) y->left->parent = x;
        y->parent = x->parent;
        if (!x->parent) root_ = y;
        else if (x == x->parent->left) x->parent->left = y;
        else x->parent->right = y;
        y->left = x;
        x->parent = y;
    }
    void right_rotate(T* y) {
        T* x = y->left;
        y->left = x->right;
        if (x->right) x->right->parent = y;
        x->parent = y->parent;
        if (!y->parent) root_ = x;
        else if (y == y->parent->left) y->parent->left = x;
        else y->parent->right = x;
        x->right = y;
        y->parent = x;
    }
    void insert_fixup(T* z) {
        while (z->parent && col(z->parent) == RbColor::RED) {
            if (z->parent == z->parent->parent->left) {
                T* y = z->parent->parent->right;
                if (col(y) == RbColor::RED) {
                    set_col(z->parent, RbColor::BLACK);
                    set_col(y, RbColor::BLACK);
                    set_col(z->parent->parent, RbColor::RED);
                    z = z->parent->parent;
                } else {
                    if (z == z->parent->right) { z = z->parent; left_rotate(z); }
                    set_col(z->parent, RbColor::BLACK);
                    set_col(z->parent->parent, RbColor::RED);
                    right_rotate(z->parent->parent);
                }
            } else {
                T* y = z->parent->parent->left;
                if (col(y) == RbColor::RED) {
                    set_col(z->parent, RbColor::BLACK);
                    set_col(y, RbColor::BLACK);
                    set_col(z->parent->parent, RbColor::RED);
                    z = z->parent->parent;
                } else {
                    if (z == z->parent->left) { z = z->parent; right_rotate(z); }
                    set_col(z->parent, RbColor::BLACK);
                    set_col(z->parent->parent, RbColor::RED);
                    left_rotate(z->parent->parent);
                }
            }
        }
        set_col(root_, RbColor::BLACK);
    }
    void transplant(T* u, T* v) {
        if (!u->parent) root_ = v;
        else if (u == u->parent->left) u->parent->left = v;
        else u->parent->right = v;
        if (v) v->parent = u->parent;
    }
    void delete_fixup(T* x, T* x_parent) {
        while (x != root_ && col(x) == RbColor::BLACK) {
            if (x == x_parent->left) {
                T* w = x_parent->right;
                if (col(w) == RbColor::RED) {
                    set_col(w, RbColor::BLACK); set_col(x_parent, RbColor::RED);
                    left_rotate(x_parent); w = x_parent->right;
                }
                if (col(w->left) == RbColor::BLACK && col(w->right) == RbColor::BLACK) {
                    set_col(w, RbColor::RED); x = x_parent; x_parent = x->parent;
                } else {
                    if (col(w->right) == RbColor::BLACK) {
                        set_col(w->left, RbColor::BLACK); set_col(w, RbColor::RED);
                        right_rotate(w); w = x_parent->right;
                    }
                    set_col(w, col(x_parent)); set_col(x_parent, RbColor::BLACK);
                    set_col(w->right, RbColor::BLACK); left_rotate(x_parent); x = root_;
                }
            } else {
                T* w = x_parent->left;
                if (col(w) == RbColor::RED) {
                    set_col(w, RbColor::BLACK); set_col(x_parent, RbColor::RED);
                    right_rotate(x_parent); w = x_parent->left;
                }
                if (col(w->right) == RbColor::BLACK && col(w->left) == RbColor::BLACK) {
                    set_col(w, RbColor::RED); x = x_parent; x_parent = x->parent;
                } else {
                    if (col(w->left) == RbColor::BLACK) {
                        set_col(w->right, RbColor::BLACK); set_col(w, RbColor::RED);
                        left_rotate(w); w = x_parent->left;
                    }
                    set_col(w, col(x_parent)); set_col(x_parent, RbColor::BLACK);
                    set_col(w->left, RbColor::BLACK); right_rotate(x_parent); x = root_;
                }
            }
        }
        set_col(x, RbColor::BLACK);
    }
    static T* min_node(T* n) { while (n && n->left) n = n->left; return n; }

    void remove_locked(T* z) {
        T* y = z; T* x; T* x_parent;
        RbColor y_orig = col(y);
        if (!z->left) { x = z->right; x_parent = z->parent; transplant(z, z->right); }
        else if (!z->right) { x = z->left; x_parent = z->parent; transplant(z, z->left); }
        else {
            y = min_node(z->right); y_orig = col(y); x = y->right;
            if (y->parent == z) { x_parent = y; }
            else {
                x_parent = y->parent;
                transplant(y, y->right);
                y->right = z->right; y->right->parent = y;
            }
            transplant(z, y);
            y->left = z->left; y->left->parent = y;
            set_col(y, col(z));
        }
        if (y_orig == RbColor::BLACK) delete_fixup(x, x_parent);
    }

public:
    void init(const char* name) { initlock(&lock_, name); }

    void insert(T* z) {
        acquire(&lock_);
        T* y = nullptr; T* x = root_;
        while (x) { y = x; x = Compare(z, x) ? x->left : x->right; }
        z->parent = y;
        if (!y) root_ = z;
        else if (Compare(z, y)) y->left = z;
        else y->right = z;
        z->left = z->right = nullptr;
        set_col(z, RbColor::RED);
        insert_fixup(z);
        release(&lock_);
    }
    void remove(T* z) { acquire(&lock_); remove_locked(z); release(&lock_); }
    T* first() { acquire(&lock_); T* n = min_node(root_); release(&lock_); return n; }

    // 최솟값 탐색+제거를 한 번에 -- first()와 remove()를 따로 부르면 그 사이에
    // 다른 코어가 같은 노드를 지울 수 있는 TOCTOU 레이스가 생김
    T* pop_first() {
        acquire(&lock_);
        T* n = min_node(root_);
        if (n) remove_locked(n);
        release(&lock_);
        return n;
    }
};
```

**설계 불변조건 — Zero Dynamic Allocation:** 트리 노드 필드(`left`/`right`/`parent`/`color`)는 별도 할당 없이 `struct proc`에 직접 임베드한다(Linux `rb_node` 임베딩, project1의 `runNode` 임베딩과 동일 패턴, §1의 POD-only 규칙과도 일치). `proc[NPROC]` 고정 배열이 이미 커널 부팅 시 한 번에 정적 확보되므로, 트리 삽입·삭제 경로에서 `kalloc`/`new`가 **단 한 번도** 호출되지 않는다. 검증 방법: 구현 후 `objdump -d`로 insert/remove/rebalance 함수 디스어셈블리에 `kalloc` 심볼 호출(`jal`/`call`)이 없는지 직접 확인.

**필드 이름을 `left`/`right`/`parent`/`color`로 (CFS 전용 이름 대신) 일반화한 이유:** Phase 2.5에서 VMA 관리자도 같은 `IntrusiveTree` 템플릿을 재사용하려면, `struct vma`도 동일한 필드 이름을 가져야 `Compare`만 바꿔서 인스턴스화할 수 있다. `cfsLeft` 같은 CFS 전용 이름을 쓰면 재사용성이 깨진다 — 약간 일반적인 이름이 되는 트레이드오프를 감수했다.

### Phase 2.5 — VMA 관리자를 증강 트리로 재구성 (Phase 2 완료를 전제로 함)

**주의:** "Phase 2 트리를 그대로 재사용한다"는 표현은 부정확하다 — CFS는 단순 정렬(최소 vruntime 추출)만 필요하지만, mmap의 실제 요구사항은 **"길이 L 이상의 빈 주소 구간을 찾아라"**라서 노드 키 비교만으론 부족하고 **서브트리 증강(augmentation)**이 필요하다. 이건 Linux의 실제 설계와 동일한 문제다 — `vm_area_struct`의 rbtree가 각 노드에 `rb_subtree_gap`(서브트리 내 최대 여유 공간)을 들고 다니는 게 정확히 이 이유다. 그래서 이 Phase의 진짜 작업은 "트리를 갖다 쓴다"가 아니라 **Phase 2의 `IntrusiveTree<T, Compare>`를 `IntrusiveTree<T, Compare, Augment>`로 일반화**하는 것이다 — CFS는 `Augment = 없음`, VMA는 `Augment = 서브트리 최대 갭 갱신 훅`으로, 같은 소스 템플릿이 두 개의 독립적으로 특수화된 인스턴스로 컴파일된다(진짜 "zero-overhead 제네릭"의 핵심 — 런타임에 코드를 공유하는 게 아니라 컴파일타임에 각각 완전히 다른 기계어로 분화됨).

**Before ([kernel/sysfile_mmap.cpp](kernel/sysfile_mmap.cpp) `sys_mmap`, Phase 1.5 반영 후 기준):**
```cpp
// O(N^2) 최악: 충돌마다 p->vmas 전체를 처음부터 재스캔
bool collision = true;
while (collision) {
    collision = false;
    for (const auto& v : p->vmas) {
        if (!v.used) continue;
        if (addr < (v.addr + v.length) && (addr + aligned_len > v.addr)) {
            addr = v.addr - aligned_len;
            collision = true;
            break;
        }
    }
}
```

**After:**
```cpp
// kernel/intrusive_tree.hpp 확장 — Compare는 Phase 2와 동일한 인터페이스, Augment만 추가
template<typename T, auto Compare, auto Augment>
struct IntrusiveTree { /* 삽입/삭제/회전마다 Augment(node)로 서브트리 메타데이터 갱신 */ };

// VMA 트리 전용 인스턴스화: 주소순 정렬 + "서브트리 내 최대 여유 공간" 증강
uint64 find_gap(IntrusiveTree<vma, vma_addr_less, vma_max_gap_update>& tree, uint64 len) {
    // 트리 한 번 서술(O(log N))로 길이 len 이상의 빈 구간을 바로 찾음
}
```

**무엇이 바뀌었는가:**

| 항목 | 주장 | 근거 |
|---|---|---|
| 알고리즘 복잡도 | O(N²) 최악(재스캔) → O(log N)(증강 트리 서술 1회) | Linux `rb_subtree_gap`과 동일한 기법, 알고리즘적으로 검증된 기법이지 실험적 발상이 아님 |
| "Zero-overhead 재사용" | 정확히는 "같은 템플릿의 서로 다른 인스턴스화" | CFS 인스턴스와 VMA 인스턴스는 컴파일타임에 완전히 분리된 기계어로 생성됨(공유 코드에 런타임 분기 없음) |

**⚠️ 측정에 대한 정직한 한계 — 반드시 문서에 남길 것:** [kernel/proc.h](kernel/proc.h)에 `#define NVMA 16`으로 확인했다. O(N²) 최악의 경우도 최대 256회 비교라, **현재 크기에서는 rdcycle/rdinstret으로 유의미한 속도차가 실측되지 않을 가능성이 높다.** "미친 듯이 빨라졌다"는 주장은 §3의 과장 금지 원칙에 위배되므로 쓰지 않는다.

**측정 방법 (재설계):** "현재 빨라졌다"가 아니라 **"N에 따라 두 알고리즘이 갈라지는 지점을 실측한다"**로 청구 범위를 바꾼다. 벤치마크 전용으로 `NVMA`를 일시적으로 키워(예: 256) N을 스윕하면서 선형탐색 vs 증강 트리의 rdinstret을 비교하고, 교차점(crossover point)을 그래프로 제시한다. 부수적으로 "xv6이 왜 원래 `NVMA`를 16으로 작게 잡았는지(O(N²)가 감당되는 한계선)"까지 설명 가능해져서 서사가 더 탄탄해진다.

**의존성:** Phase 2가 먼저 완료돼야 하고(트리 인프라 재사용), 순서상 Phase 2 직후에 붙인다. Phase 1.5/1.6/1.7과 달리 독립 실행 불가능한 Phase다.

### Phase 3 — 멀티스레딩

**참고 자료 발견 (2026-08-27):** `project2-braeden-hue`(같은 사용자의 이전 HYU-ELE3021 과제)에 이미 `sys_clone`/`sys_join`, `CLONE_VM` 플래그, `tgid`/`group_leader`(스레드 그룹), `ustack`(스레드용 유저 스택 베이스)까지 구현된 스레드 생성 코드가 있다. Phase 3 착수 시 project1의 CFS처럼 이걸 먼저 읽고 포팅 대상으로 삼을 것 — 아직 상세 검토는 안 함, Phase 3 착수 시 project1과 동일한 절차(읽기 → 버그/락 검토 → 매핑표 작성)를 반복.

**Before:** `fork()`만 존재, 주소공간 공유 스레드 없음. `sleep()`/`wakeup()`([kernel/proc.c](kernel/proc.c))은 채널에서 자는 프로세스를 **전부** 깨우는 broadcast 구조 (thundering herd).

**⚠️ 설계 변경 (실측 후 정정):** 최초 계획은 `std::atomic<int>::wait/notify_one`으로 대기 큐를 짜는 것이었으나, 이 프로젝트의 실제 freestanding 플래그(`-ffreestanding -nostdlib`, [Makefile](Makefile))로 직접 컴파일해보니 **`wait`/`notify_one` 멤버 자체가 선언되지 않는다** (`error: 'struct std::atomic<int>' has no member named 'wait'`). libstdc++ 헤더가 이 API를 hosted 스레드 지원 매크로 뒤에 가드해두고, freestanding 크로스 빌드엔 그게 없어서 링크 이전에 컴파일 단계에서 이미 막힌다 — futex syscall을 부르는 hosted 전용 기능이라 커널 자체 구현엔 애초에 못 쓴다.

반면 `compare_exchange_weak`/`fetch_add` 등 저수준 CAS 연산은 실측 결과 완전히 다르다 — 실제로 `lr.w.aqrl`/`sc.w.rl`(CAS), `amoadd.w.aqrl`(fetch_add) 같은 RISC-V AMO 명령어로 그대로 코드생성되고, 미해결 심볼이 0개다(`nm -u` 확인). 이건 순수 컴파일러 내장 코드생성이라 libstdc++ 런타임이 전혀 필요 없다.

**After (수정된 설계):**
- 커널: `sys_clone` syscall (C, `sysproc.c`) — 주소공간/파일테이블 공유 프로세스 생성. 이 부분은 트랩/레지스터 저장 로직이라 **C로 유지** (C++로 바꿔도 이득 없음, 트랩 진입 코드는 정적 디스패치 대상이 아님).
- 대기 큐: `kernel/waitqueue.hpp` — `std::atomic<int>`의 CAS(`compare_exchange_weak`)로 락-프리 상태 전이(락 획득 시도, "대기자 있음" 플래그)만 처리하고, **실제 블로킹은 xv6의 기존 `sleep()/wakeup()`을 그대로 재사용**하는 얇은 래퍼로 구현 — futex를 새로 만드는 게 아니라 "xv6 sleep/wakeup 위에 CAS로 정밀한 wake 대상 선정을 얹는" 구조. broadcast wake(채널의 모든 대기자를 깨우는 현재 xv6 방식, thundering herd)를 CAS로 보호되는 상태값 기반으로 정확히 한 스레드만 깨우도록 교체 → 불필요한 컨텍스트 스위치 감소가 측정 대상. **§1의 POD-only 규칙 적용**: `std::atomic<int>`를 `struct proc`의 필드로 넣지 않는다 — POD `int` 필드를 두고 그 위에서 지역적으로 원자 연산을 적용하는 방식으로 설계할 것.
- 유저스페이스: `user/cpp/thread.hpp` — RAII `Thread`(소멸자에서 join 강제), `ScopedLock` — `std::thread`/`std::lock_guard` 스타일 래퍼. syscall 위에 얇게 얹는 것이라 자유도 높음(freestanding 제약 거의 없음).

### Phase 4 — mmap 확장: COW fork (별도 최적화 축)

**주의:** 이건 Phase 1의 "정적 디스패치 오버헤드 제거" 명제와 무관한 **별개의 명제**다 — "락 세분화로 동시성 처리량 개선"을 증명하는 자리.

**Before:** 없음(신규 기능). 일반적인 C 구현이라면 페이지 refcount를 전역 스핀락 하나로 보호.

**After:** `kernel/cow.hpp` — 페이지 프레임 refcount를 `std::atomic<int>`로 관리, 전역 락 대신 페이지 단위 원자적 증감. 측정은 rdcycle/rdinstret이 아니라 **멀티코어 fork 처리량 벤치마크**(코어 수 대비 처리량)로 해야 함 — Phase 0~3와 방법론이 다르다는 걸 문서에 명시.

### Phase 5 — 벤치마크/시각화 (포트폴리오 산출물)

- `user/cpp/bench_harness.cpp` — 정책별(RR/FCFS/CFS) 처리량·응답시간·공평성 측정, N회 반복+중앙값 방법론 적용
- 결과를 objdump diff와 함께 정리 — Phase 1~4 각각의 "Before/After" 문서 산출물로 묶음 (아래 3장 규칙 적용)

## 3. Before/After 비교 문서 작성 규칙 (모든 Phase에 공통 적용)

각 Phase를 실제로 구현한 뒤 문서화할 때는 아래 5개 항목을 **반드시 이 순서로** 채운다. 이후 대화에서 코드를 작성할 때도 이 형식을 기본값으로 따른다.

1. **Before (C)** — 실제 파일:라인 링크 + 코드 스니펫. "일반적으로 이렇게 짰을 것"이 아니라 실제 이 저장소에 있던/있었을 코드 기준.
2. **After (C++)** — 실제 파일:라인 링크 + 코드 스니펫.
3. **무엇이 바뀌었는가 (1줄)** — 반드시 아래 매핑 중 하나로 명명한다:
   | C 스타일 | → | C++ 대체 |
   |---|---|---|
   | 함수포인터 테이블 (`struct ops { int(*fn)(void); }`) | → | concepts + 템플릿 정적 디스패치 |
   | `void*` + 캐스팅 제네릭 | → | 템플릿 |
   | 포인터+길이 인자 쌍 | → | `std::span` |
   | 필드 순서 의존 구조체 초기화 | → | designated initializer |
   | `<`,`>`,`==`... 개별 비교 함수 | → | `operator<=>` |
   | 매크로 상수표 / 부팅 시 런타임 루프 계산 | → | `constexpr`/`consteval` |
   | 전역 생성자 순서 의존 | → | `constinit` |
   | 전역 락 하나로 보호되는 refcount | → | `std::atomic<int>` (단, §1 POD-only 규칙상 지역변수/POD-필드+지역연산 조합으로만) |
4. **측정** — Phase 0~3은 rdinstret(정적+동적) + objdump diff, Phase 4는 멀티코어 처리량 벤치마크. 방법론을 반드시 못박고, "무엇을 측정 못 했는지"도 같이 적는다.
5. **한계** — QEMU TCG에서 증명 불가능한 부분(분기예측, 캐시미스 등)을 항상 명시. 과장 금지: "N사이클 절감"이 아니라 "N명령어 감소로 인한 오버헤드 제거"처럼 정확한 인과관계로 서술.

## 4. Makefile 통합 시 필요한 작업

- `$K/%.o: $K/%.cpp` 규칙 (완료, [Makefile](Makefile))
- `OBJS`에 신규 `.o` 추가 (Phase마다 계속)
- C↔C++ 경계는 전부 `extern "C"`로 명시 (name mangling 방지) — **주의: 전방선언과 정의 둘 다에 필요함, 하나만 하면 링크 충돌 에러 (Phase 0 실측 확인)**
- 여러 C++ 파일이 같은 C 헤더를 include할 수 있으므로, 그 헤더엔 `#pragma once`가 있어야 함(원본 C 헤더 대부분엔 없었음 — 필요해질 때마다 추가, [kernel/proc.h](kernel/proc.h)가 첫 사례, Phase 1)
- `extern` 배열/구조체 선언은 반드시 해당 타입의 **완전한 정의 뒤에** 위치해야 함(incomplete type 에러, Phase 1 실측 확인)
- 기존 C 함수가 `__attribute__((noreturn))`이면, 그걸 대체하는 C++ 함수 체인 전체를 `[[noreturn]]`으로 표시해야 함(안 그러면 "noreturn 함수가 리턴한다" 에러, Phase 1 실측 확인)
- `make analyze`로 정적 분석(Phase 0.5)

## 5. 순서 요약

Phase 0(벤치마크 인프라) → Phase 0.5(정적 분석 인프라) → Phase 1(스케줄러 정책, FCFS→RR→CFS) → Phase 2(템플릿 트리) → Phase 2.5(VMA 증강 트리 재구성) → Phase 3(멀티스레딩) → Phase 4(mmap COW, 별도 축) → Phase 5(벤치마크/포트폴리오 문서화)

Phase 0과 Phase 1의 FCFS까지는 순서를 반드시 지킨다(측정 인프라 없이 "빨라졌다" 주장 불가) — **완료됨**. Phase 0.5는 도구 채택이라 아무 때나 해도 무방하지만 이후 모든 신규 `.cpp`에 관례로 적용하는 게 목적이라 빠를수록 좋다 — **완료됨**. Phase 1.5(mmap 타입 안전성) → Phase 1.6(RefPtr/ErrorOr) → Phase 1.7(Number\<T\> narrowing 방지)은 이 순서로 진행한다(1.6이 1.5의 gap을, 1.7이 1.6의 `ErrorOr`을 이어받는 구조). Phase 2.5는 **Phase 2 완료가 전제**라 독립 실행 불가능하고 반드시 Phase 2 이후에 온다(트리 인프라를 확장해서 재사용하는 구조). Phase 0.5/1.5/1.6/1.7과 Phase 4는 나머지 Phase와 독립적이라 아무 때나 끼워 넣어도 무방하다.

## 6. Contemporary C++ 5대 특성과 프로젝트 매핑 (포트폴리오 핵심 표)

이 프로젝트가 "C++을 썼다"가 아니라 "Modern C++의 특성 하나하나를 의도적으로, 실측 가능한 형태로 증명했다"는 걸 보이는 게 최종 목표다. 아래 표는 각 특성이 정확히 어느 Phase의 어떤 실측 결과로 증명되는지 연결한다 — 포트폴리오 문서의 목차로 그대로 써도 되는 구조다.

| 특성 (Stroustrup) | 증명 위치 | 실측 증거 |
|---|---|---|
| **정적 타입 안전성** | Phase 1.5 (mmap designated init) | 선언 순서 위반 시 컴파일 에러 재현 (`error: designator order...`) |
| | Phase 1 (스케줄러 concepts) | 정책이 `SchedulerPolicy` 인터페이스를 안 지키면 런타임이 아니라 컴파일 타임에 에러 |
| | Phase 1.6 (`ErrorOr<T>`) | "값 또는 에러"를 타입으로 강제 — 에러 체크 누락이 런타임 사고가 아니라 타입 사용 문제로 드러남 |
| | Phase 1.7 (`Number<T>`, Stroustrup 2025 §2 직접 인용) | 1차 문헌 근거 — narrowing 방지를 Phase 1.5의 우연한 catch에서 프로젝트 전체의 체계적 방어로 확장 |
| | Phase 0.5 (`-fanalyzer`) | 이미 구현된 Phase 0/1.5 코드에 실측 적용, findings 0개 확인 — 앞으로 모든 신규 `.cpp`에 적용할 관례로 채택 |
| **자원 안전성 (no leak)** | ⚠️ Phase 1.6에서 시도했으나 구조체 임베딩이 불가능해 **부분 기각됨** — `RefPtr`은 함수 지역변수로만 유효, `struct vma` 필드 차원의 자원 안전성은 미해결로 남음(정직하게 표기) | `proc.h`가 커널 전체에 include되는 구조상, 클래스 타입 필드를 공유 구조체에 넣을 수 없음이 실측으로 확인됨 |
| | Phase 2 (인트루시브 트리) | Zero Dynamic Allocation — `objdump`로 `kalloc` 호출 부재 확인 |
| | Phase 3 (유저스페이스 `Thread`/`ScopedLock`) | RAII로 join/unlock 누락을 소멸자가 강제 |
| **하드웨어 직접 제어** | Phase 0 (CSR `enum class`) | `Mcounteren::CY \| TM \| IR` → 실제 레지스터 값 `0b111`로 그대로 코드생성 (objdump에서 `ori a5,a5,7` 확인) |
| | Phase 3 (CAS) | `compare_exchange_weak`/`fetch_add`가 `lr.w.aqrl`/`sc.w.rl`/`amoadd.w.aqrl` RISC-V AMO 명령어로 직결 (실측 확인, 미해결 심볼 0개) |
| **효율성 (zero-overhead)** | Phase 1 (정적 디스패치) | rdinstret + objdump로 간접호출(`ld`+`jalr`) 제거를 명령어 수 감소로 증명 (하니스는 Phase 5에서 완성) |
| | Phase 0 (constexpr 폴딩) | 여러 constexpr/if consteval 조합이 단일 `li` 명령어로 완전히 상수 폴딩됨을 objdump로 확인 |
| | Phase 1.7 (`if constexpr` narrowing 필터) | 논문 §2.1 인용 — narrowing이 애초에 불가능한 조합은 컴파일타임에 검사 자체가 사라짐, 실제 값 검사가 필요한 조합에서만 런타임 비용 발생 |
| | Phase 2.5 (증강 트리, VMA 관리) | 같은 `IntrusiveTree` 템플릿이 CFS·VMA에서 서로 다른 기계어로 분화 + O(N²)→O(log N) 알고리즘 개선. **단, `NVMA=16`에서는 속도차가 실측 안 될 수 있음을 정직하게 명시**, N-스윕 벤치마크로 청구 범위 재정의 |
| **안정성/호환성** | 프로젝트 전체 아키텍처 | `kernel/proc.c` 등 기존 `.c` 파일은 로직을 재작성하지 않고, `extern "C"` 경계로 신규 `.cpp` 모듈만 얹는 구조(§4) — "전면 재작성 없이 기존 C 자산과 공존"이 Stroustrup이 말하는 안정성 원칙의 실천 |

**정직성 원칙:** 이 표에 "해당 없음"이나 "미착수"로 표기된 항목을 나중에 슬쩍 지우지 말 것 — 실제로 못 채운 특성이 있다는 걸 보여주는 게, 5개를 억지로 다 채운 것처럼 포장하는 것보다 포트폴리오 신뢰도에 낫다.
