#pragma once
// EP1-RESOURCE-01: trivial helper shared identically by A/B/C so its
// presence isn't itself a variable in the comparison. "Uses" an acquired
// resource (one volatile byte write) so no compiler can fold the
// acquire/release pair away as dead code -- same anti-elimination
// discipline as BENCH_KEEP in kernel/bench.cpp, applied to memory instead
// of a register value since these resources are pages/slots, not a
// register-sized return value.
#define EP1_TOUCH(p) (*(volatile unsigned char*)(p) = 1)

// Return codes shared by every A/B/C entry point (real and fake backend,
// timed and test-mode alike) -- documented once here instead of per file.
//   0 = success, both resources handed off via *out1/*out2
//   1 = first acquisition failed, *out1=*out2=0
//   2 = second acquisition failed (first already released), *out1=*out2=0
//   3 = explicit early failure after both acquisitions succeeded, both
//       released in reverse acquisition order, *out1=*out2=0
#define EP1_OK 0
#define EP1_FAIL_FIRST 1
#define EP1_FAIL_SECOND 2
#define EP1_EARLY_FAIL 3
