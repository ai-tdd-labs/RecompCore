// RecompCore per-game native module export glue (game id set at build time).
//
// Wraps the DolRecomp-generated constant-time chunk dispatcher behind the
// StaticRecomp module ABI. All environment access goes through the CPUState
// hook pointers the chassis installs; this dylib has no host dependencies.

#include "generated.h"

#include "StaticRecompABI.h"

#include <stdatomic.h>

static atomic_uint s_host_event_id;
static atomic_ullong s_host_event_timebase;

void moderngekko_module_signal_host_event(CPUState* ctx, u32 event_id)
{
    if (!ctx || event_id == 0)
        return;

    // Publish the id last. The host's acquire exchange then observes the
    // matching guest timebase without putting anything in the dispatch path.
    atomic_store_explicit(&s_host_event_timebase, ctx->timebase, memory_order_relaxed);
    atomic_store_explicit(&s_host_event_id, event_id, memory_order_release);
}

static int chassis_dispatch(CPUState* ctx, u32 address)
{
    // Sparse module hooks are compiled into their exact generated PC labels.
    // Dispatch therefore stays identical for patched and unpatched modules.
    return dolrecomp_dispatch(ctx, address);
}

static void chassis_on_state_loaded(CPUState* ctx)
{
    // Re-arm host FP rounding/flush state from the freshly loaded guest FPSCR.
    ppc_fpscr_updated(ctx);
}

#include "module_tables.inc"

static const StaticRecompModuleDesc s_desc = {
    STATICRECOMP_ABI_VERSION,
    GXRUNTIME_CPU_ABI_VERSION,
    (u32)sizeof(CPUState),
    MODULE_GAME_ID,
    DOLRECOMP_ENTRY_POINT,
    chassis_dispatch,
    chassis_on_state_loaded,
    s_code_ranges,
    MODULE_CODE_RANGE_COUNT,
    s_smc_ranges,
    MODULE_SMC_RANGE_COUNT,
    s_chunk_ranges,
    MODULE_CHUNK_RANGE_COUNT,
    s_chunk_hashes,
    s_chunk_functions,
};

#if defined(_WIN32)
#define RECOMP_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define RECOMP_MODULE_EXPORT __attribute__((visibility("default")))
#else
#define RECOMP_MODULE_EXPORT
#endif

RECOMP_MODULE_EXPORT const StaticRecompModuleDesc* staticrecomp_get_module(void)
{
    return &s_desc;
}

RECOMP_MODULE_EXPORT bool staticrecomp_take_host_event(StaticRecompHostEvent* event)
{
    if (!event)
        return false;

    const u32 id = atomic_exchange_explicit(&s_host_event_id, 0, memory_order_acq_rel);
    if (id == 0)
        return false;

    event->id = id;
    event->reserved = 0;
    event->guest_timebase =
        (u64)atomic_load_explicit(&s_host_event_timebase, memory_order_relaxed);
    event->core_ticks = 0;
    return true;
}
