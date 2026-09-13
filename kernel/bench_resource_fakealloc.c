// EP1-RESOURCE-01: fixed-slot fake allocator. Its only job is to be a cheap,
// non-kalloc resource so the TIMED experiment can isolate "cost of the
// ownership-management mechanics themselves (branches, moves, destructor
// calls)" from "cost of a real kalloc()/kfree() call (free-list, lock)".
// Never pooled with the real-kalloc numbers -- reported as a separate
// backend throughout. Plain C, shared identically by all three conditions'
// fake-backend timed entry points, so it cannot bias the A-vs-B-vs-C
// comparison the way a per-condition allocator would.
#include "types.h"

#define FAKE_NSLOTS 4
#define FAKE_SLOT_BYTES 8

static unsigned char fake_used[FAKE_NSLOTS];
static unsigned char fake_mem[FAKE_NSLOTS][FAKE_SLOT_BYTES];

void fake_reset(void) {
    for (int i = 0; i < FAKE_NSLOTS; i++) fake_used[i] = 0;
}

void* fake_alloc(void) {
    for (int i = 0; i < FAKE_NSLOTS; i++) {
        if (!fake_used[i]) {
            fake_used[i] = 1;
            return &fake_mem[i][0];
        }
    }
    return 0;
}

void fake_free(void* p) {
    unsigned char* base = &fake_mem[0][0];
    long off = (long)((unsigned char*)p - base);
    int idx = (int)(off / FAKE_SLOT_BYTES);
    if (idx >= 0 && idx < FAKE_NSLOTS) {
        fake_used[idx] = 0;
    }
}
