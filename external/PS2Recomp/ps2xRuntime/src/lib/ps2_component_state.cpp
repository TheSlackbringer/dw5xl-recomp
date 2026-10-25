// DW5XL (issue 027): save-state serializers of the runtime's components (EE memory and registers, GS and its
// renderer, VU). Kept apart from ps2_save_state.cpp, which drives the whole runtime: tools that link only the GS
// (dw5xl_gsrender) must not pull in PS2Runtime and the recompiled function table.
#include "ps2_runtime.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_vu1.h"
#include "runtime/state_archive.h"

void PS2Memory::serializeState(ps2x::StateArchive &a)
{
    auto transfers = [](ps2x::StateArchive &x, std::vector<PendingTransfer> &list)
    {
        x.sequence(list, [](ps2x::StateArchive &y, PendingTransfer &t)
        {
            y.value(t.fromScratchpad);
            y.value(t.srcAddr);
            y.value(t.qwc);
            y.vector(t.chainData);
        });
    };
    a.tag("EMEM");
    a.raw(m_rdram, PS2_RAM_SIZE);
    a.raw(m_scratchpad, PS2_SCRATCHPAD_SIZE);
    a.raw(iop_ram, 2u * 1024u * 1024u);
    a.raw(m_gsVRAM, PS2_GS_VRAM_SIZE); // the CPU-side copy; the renderer's own VRAM is its own section
    a.raw(m_vu0Code, PS2_VU0_CODE_SIZE);
    a.raw(m_vu0Data, PS2_VU0_DATA_SIZE);
    a.raw(m_vu1Code, PS2_VU1_CODE_SIZE);
    a.raw(m_vu1Data, PS2_VU1_DATA_SIZE);
    a.tag("EREG");
    for (uint64_t *reg : {&gs_regs.pmode, &gs_regs.smode1, &gs_regs.smode2, &gs_regs.srfsh, &gs_regs.synch1,
                          &gs_regs.synch2, &gs_regs.syncv, &gs_regs.dispfb1, &gs_regs.display1, &gs_regs.dispfb2,
                          &gs_regs.display2, &gs_regs.extbuf, &gs_regs.extdata, &gs_regs.extwrite, &gs_regs.bgcolor,
                          &gs_regs.imr, &gs_regs.busdir, &gs_regs.siglblid})
        a.value(*reg);
    for (std::atomic<uint64_t> *reg : {&gs_regs.csr, &gs_regs.vsyncTick})
    {
        uint64_t v = reg->load(std::memory_order_acquire);
        a.value(v);
        if (a.loading())
            reg->store(v, std::memory_order_release);
    }
    a.value(vif0_regs);
    a.value(vif1_regs);
    a.value(dma_regs);
    a.podMap(m_ioRegisters);
    a.vector(m_tlbEntries);
    a.value(m_seenGifCopy);
    a.value(m_path3Masked);
    a.value(m_vif1PendingPath2ImageQwc);
    a.value(m_vif1PendingPath2DirectHl);
    a.sequence(m_path3MaskedFifo, [](ps2x::StateArchive &x, std::vector<uint8_t> &packet) { x.vector(packet); });
    transfers(a, m_pendingGifTransfers);
    transfers(a, m_pendingVif0Transfers);
    transfers(a, m_pendingVif1Transfers);
    {
        std::lock_guard lock(m_completedDmacMutex);
        a.vector(m_completedDmacCauses);
    }
    a.sequence(m_codeRegions, [](ps2x::StateArchive &x, CodeRegion &r)
    {
        x.value(r.start);
        x.value(r.end);
        x.boolVector(r.modified);
    });
    a.value(m_eeTimers);
    a.value(m_ipuInputCredit);
    a.value(m_ipuInputTagLoaded);
    a.value(m_ipuInputEndAfterPayload);
    if (a.loading())
    {
        // VU code changed under any cache built from it: let lifted VU1 / interpreters look again.
        markVU0CodeModified();
        markVU1CodeModified();
    }
}

void GSRasterBackend::SerializeState(ps2x::StateArchive &)
{
    throw ps2x::StateError("this renderer does not support save states (use the Vulkan renderer)");
}

void GS::serializeState(ps2x::StateArchive &a)
{
    m_forceFieldRead.store(true, std::memory_order_release); // issue 023 2R: read the next field after a load
    a.tag("GSFE");
    a.value(m_gifPathStates);
    a.value(m_localMemorySize);
    a.value(m_ctx);
    a.value(m_prim);
    a.value(m_primRegister);
    a.value(m_prmodeRegister);
    a.value(m_curR);
    a.value(m_curG);
    a.value(m_curB);
    a.value(m_curA);
    a.value(m_curU);
    a.value(m_curV);
    a.value(m_curFog);
    a.value(m_fogR);
    a.value(m_fogG);
    a.value(m_fogB);
    a.value(m_prmodecont);
    a.value(m_pabe);
    a.value(m_scanmsk);
    a.value(m_dimx);
    a.value(m_dthe);
    a.value(m_colclamp);
    a.value(m_texa);
    a.value(m_texclut);
    a.value(m_bitbltbuf);
    a.value(m_trxpos);
    a.value(m_trxreg);
    a.value(m_trxdir);
    a.value(m_vtxQueue);
    a.value(m_vtxCount);
    a.value(m_vtxIndex);
    a.value(m_lastDisplayBaseBytes);
    a.value(m_preferredDisplaySourceFrame);
    a.value(m_preferredDisplayDestFbp);
    a.value(m_hasPreferredDisplaySource);
    if (a.loading())
    {
        // Host presentation is rebuilt from the next fields.
        std::lock_guard lock(m_snapshotMutex);
        m_displaySnapshot.clear();
        m_latchedFrame = {};
        m_hasHostPresentationFrame = false;
        ++m_hostPresentationGeneration;
        std::lock_guard fieldLock(m_fieldScanoutMutex);
        m_hasSubmittedField = false;
    }
    if (!m_backend)
        throw ps2x::StateError("no GS renderer");
    m_backend->SerializeState(a);
}

void VU1Interpreter::serializeState(ps2x::StateArchive &a)
{
    a.tag(m_unit == Unit::VU0 ? "VU0S" : "VU1S");
    a.value(m_state);
    a.value(m_flagPipeline);
    a.value(m_fdiv);
    a.value(m_efu);
    a.value(m_storePipeline);
    a.value(m_xgkick);
    a.value(m_ready);
    a.value(m_cycle);
    a.value(m_efuResourceReady);
    a.value(m_nextCommitCycle);
    a.value(m_writebackHorizon);
    a.value(m_flagMask);
    a.value(m_efuMask);
    a.value(m_storeMask);
    a.value(m_workingClip);
    a.value(m_currentUpperInstruction);
    a.value(m_fmacQ);
    a.value(m_fmacI);
    a.value(m_viBranchBackupValue);
    a.value(m_viBranchBackupReg);
    a.value(m_viBranchBackupValid);
    a.value(m_stopRequested);
    a.value(m_pendingHaltD);
    a.value(m_pendingHaltT);
    if (a.loading())
    {
        // Decoded code and lift timing are rebuilt from the restored VU code memory.
        m_decodedCodeCacheValid = false;
        m_liftTimingValid = false;
        m_cachedVuCode = nullptr;
        m_cachedMemory = nullptr;
        m_cachedCodeSize = 0;
        m_cachedCodeGeneration = 0;
    }
}
