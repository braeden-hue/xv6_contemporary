# Modern xv6 OS

**Applying Modern C++ to OS Design and Evaluation**

2026-09-14 기준 · [상세 실험 보고서](report.md)

Modern C++로 xv6의 정책 인터페이스와 자원 소유권을 명시적으로 표현하고, 그 실행 비용·안전성·스케줄링 효과를 분리해 평가한 프로젝트다.

2025년 3월 스페인 UC3M(Leganés) 교환학생 기간에 들은 Bjarne Stroustrup의 Contemporary C++ 강연에서 출발했다. "현대 C++를 운영체제에 적용하면 무엇이 좋아지는가?"라는 질문을 구현과 대조 실험으로 구체화했다.

| 에피소드 | 연구 질문 |
|---|---|
| **Ep1 · Modern C++ Abstractions: Cost and Resource Safety** | 정책 분리와 소유권 표현은 어떤 오류를 예방하며, 실행 비용은 얼마인가? |
| **Ep2 · Scheduling Policies in Modern C++: Latency and Trade-offs** | 우선순위 정책은 짧은 작업의 응답시간을 줄이며, 그 대가는 무엇인가? |

**Ep1은 언어·추상화의 비용을, Ep2는 정책의 효과를 다룬다.** 명령어 수 감소를 실행시간 감소로, 정책에 의한 응답 개선을 C++ 자체의 효과로 해석하지 않는다.

## 구조

![스케줄러 구조](docs/figures/00_scheduler_architecture.png)

*드라이버뿐 아니라 short/long 프로세스도 trap·syscall 경로로 커널에 진입한다.*

스케줄러 공통 실행부와 정책 선택 로직을 분리했다. `SchedulerPolicy` concept가 정책 인터페이스를 검사하고, `dispatch<P>()`가 빌드에서 선택한 정책 타입을 사용한다. `pick_next()`는 실행 대상을, `should_preempt()`는 타이머 경로에서 양보 여부를 결정한다.

- [정책 인터페이스](kernel/sched_policy.hpp) · [공통 실행부](kernel/scheduler.cpp)
- [자원 소유권 객체](kernel/unique_page.hpp)
- [혼합 워크로드 드라이버](user/mixbench.c)

## Ep1 — Modern C++ Abstractions: Cost and Resource Safety

같은 RR 탐색 알고리즘을 **직접 C**, **C++ 템플릿(noinline)**, **C++ 함수 포인터**로 각각 구현해 out-of-line·동일 최적화 조건에서 비교했다 — 직접 C와 C++ noinline 구현은 total instret(106,405/200회 = 532.025)까지 완전히 일치했다. 이어 페이지 소유권을 다루는 **RAII 객체(`UniqueResource`)**를 수동 C/C++ 관리와 대조했다: 정상 스코프 종료·조기 반환에서 해제를 자동화하고 복사를 컴파일 단계에서 차단했지만, 측정 경로에서 ±1 instret의 실행 명령어 차이와 진입 함수 코드 크기 증가(예: early_fail 경로 94B→104B)가 함께 관측됐다.

> 측정한 조건에서 정책 분리·소유권 표현은 관측 가능한 실행 비용 범위 안에서 특정 오류를 예방했다 — C++가 더 빠르다는 결과는 아니다.

## Ep2 — Scheduling Policies in Modern C++: Latency and Trade-offs

short task 10개·long task 2개의 고정 도착 스케줄에서 RR·SelectOnly·Preempt를 비교했다. 우선순위 기반 두 정책 모두 short 응답 median을 **2→1 tick**으로 낮췄지만, makespan·long 작업 완료 지연에 대한 사전 비용 기준을 모든 라운드에서 충족해야 한다는 요건은 만족하지 못했다. 결과 수집 방식 자체가 다른 작업의 실행을 지연시킬 수 있음을 확인해 수집 조건(V/VD/QD)을 별도로 대조했고, `-icount` 조건에서 정책별 3회씩 관측값의 재현성도 확인했다.

> 우선순위 정책의 응답 개선과 완료 비용의 상충을 관찰했다. C++ 자체가 응답시간을 줄였다는 결과로 해석하지 않는다.

### Budget 후속 탐색 — 본비교 보류

SelectOnly에 집계 실행 예산(5 tick / 10 tick)을 추가했다. 별도 양성 대조에서는 **차감 49회·소진 6회·LS 대기 중 Normal 선택 18회**를 관측했지만, 원 workload의 QD 파일럿에서는 세 값 모두 0이었다.

tick 사이의 짧은 실행을 확인하기 위해 가상 시간 구간을 관측하고 우선순위 분류 결함을 수정했다. 최종 기록에서 최대 윈도우 LS 시간은 **218,468 r_time 단위**, LS 합계는 **567,755**였다. 경험적 평균 tick 간격으로 환산한 5 tick의 참고 규모 대비 각각 약 **4.35%·11.30%**다. time 윈도우와 tick 예산 윈도우가 동일한 것은 아니며, 실제 하드웨어 시간으로도 해석하지 않는다.

**현재 workload·5/10 설정의 5라운드 본비교는 진행하지 않았다.** Budget 일반의 효과 부재가 아니라, 이 조건에서 정책 개입을 평가할 근거가 부족하다는 판단이다. 기존 결과와 오류 로그는 보존하고, 관측 토글은 OFF로 복귀했다.

시행착오(커널 재빌드 누락, 수집 간섭, 원장 해제 순서 검증 부재 등)와 상세 수치·근거 자료·결론·후속 연구 질문은 [`report.md`](report.md)에 정리했다.

## 실행 방법

빌드는 GCC 14 RISC-V 크로스 툴체인과 C++23 커널 소스 기준으로 구성되어 있다. C++ 파일은 `-ffreestanding`, `-fno-exceptions`, `-fno-rtti`를 포함한 freestanding 제약으로 컴파일된다.

요구 사항:

- RISC-V GCC/G++ 14 크로스 툴체인
- QEMU >= 7.2

xv6 실행:

```bash
make qemu
```

스케줄러 계측 회귀 테스트(xv6 내부에서):

```
$ usertests -q
$ statstest
```

C++ 커널 파일에 대한 GCC 정적 분석:

```bash
make analyze
```

make qemu는 부팅 명령이며 모든 실험을 자동 재현하지 않는다. 정책·벤치마크 토글·CPU 수·QEMU 시간 조건은 각 빌드 로그와 manifest를 확인해야 한다. 자원 실험을 끈 CPUS=3 일반 TCG 회귀에서 PASS ALL TESTS를 확인했다.

Makefile은 위 벤치마크에 사용한 디스어셈블리·심볼 파일도 함께 생성한다. Episode 2의 원시 로그·집계 스크립트·manifest는 [`docs/bench/`](docs/bench/)에 보존되어 있다.

## 기반 프로젝트와 라이선스

이 저장소는 MIT 6.1810 교육용 운영체제인 [MIT xv6-riscv](https://github.com/mit-pdos/xv6-riscv)를 기반으로 한다.

원본 xv6의 저작권과 라이선스는 [`LICENSE`](LICENSE)에 그대로 보존되어 있다.
