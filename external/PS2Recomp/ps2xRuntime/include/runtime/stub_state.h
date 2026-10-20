#pragma once
// DW5XL (issue 027): state of the HLE stubs and syscalls for save states. The stubs keep their state in globals local
// to their source files (some headers even give every including file its own copy), so each source registers its
// own serializer here at start-up; serializeStubStates runs them in name order, so a state always has the same layout.
#include "ps2x/state_archive.h"

namespace ps2x
{
    using StubStateFn = void (*)(StateArchive &);
    bool registerStubState(const char *name, StubStateFn fn);
    // rdram: this run's guest memory, for stub state that keeps host pointers into it (stateGuestRdram()).
    void serializeStubStates(StateArchive &archive, uint8_t *rdram);
    uint8_t *stateGuestRdram();
}

// The translation unit's own file (a unity batch counts as one), for names that are unique per copy of a header.
#if defined(__BASE_FILE__)
#define PS2X_STATE_TU_NAME __BASE_FILE__
#else
#define PS2X_STATE_TU_NAME __FILE__
#endif
