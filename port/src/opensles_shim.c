/* opensles_shim.c — minimal OpenSL ES 1.0 object model for the port.
 *
 * The engine only imports slCreateEngine; everything else goes through
 * vtables of objects we hand back.  Layouts below follow the Khronos
 * OpenSLES.h 1.0 structs, which the engine's call sites confirm:
 *   SLObjectItf_  Realize(0) Resume(8) GetState(0x10) GetInterface(0x18)
 *                 RegisterCallback(0x20) AbortAsyncOperation(0x28)
 *                 Destroy(0x30) SetPriority(0x38) GetPriority(0x40)
 *                 SetLossOfControlInterfaces(0x48)
 *   SLEngineItf_  CreateAudioPlayer(0x10) CreateOutputMix(0x38)
 *   SLPlayItf_    SetPlayState(0)
 *   SLAndroidSimpleBufferQueueItf_ Enqueue(0) Clear(8) GetState(0x10)
 *                 RegisterCallback(0x18)
 * (observed at img+0xeb544..0xeb774: engine creates OutputMix + a PCM
 *  AndroidSimpleBufferQueue player, registers a feeder callback, sets
 *  PLAYING and enqueues 4096 bytes.)
 *
 * v1 is SILENT: Enqueue accepts and counts buffers but no callback ever
 * fires, so the engine's feeder simply idles.  The whole game runs; audio
 * output is future work (CoreAudio sink behind the same object model).
 *
 * Interface-ID data symbols (SL_IID_ENGINE...) are PLT-style externals:
 * guest code does ldr x9,[got]; ldr x1,[x9] — so the GOT slot must hold
 * the ADDRESS of a pointer variable whose value is the UUID pointer.
 * We register &sl_iid_*_var as data; pb_relocate wires the slots, and
 * pb_opensl_fixup_slots() force-writes them post-wiring as a safety net.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pb.h"

typedef uint32_t SLuint32;
typedef int32_t  SLint32;

#define SL_RESULT_SUCCESS              0x00000000u
#define SL_RESULT_PARAMETER_INVALID    0x00000002u
#define SL_RESULT_MEMORY_FAILURE       0x00000004u
#define SL_RESULT_FEATURE_UNSUPPORTED  0x0000000Cu
#define SL_OBJECT_STATE_UNREALIZED     0x00000001u
#define SL_OBJECT_STATE_REALIZED       0x00000002u
#define SL_PLAYSTATE_STOPPED           0x00000001u
#define SL_TIME_UNKNOWN                0xFFFFFFFFu

/* ------------------------------------------------ interface ID data vars */
/* UUID contents are ours alone (nothing compares them but this file); what
 * matters is that each SL_IID_* externally-visible variable holds a unique
 * non-NULL pointer, exactly like the real libOpenSLES. */
static const uint32_t sl_uuid_engine[4] = { 0x454e4731u, 0x00010001u, 0x9e3779b9u, 0x00000001u };
static const uint32_t sl_uuid_play[4]   = { 0x504c4159u, 0x00010001u, 0x9e3779b9u, 0x00000002u };
static const uint32_t sl_uuid_bq[4]     = { 0x42465131u, 0x00010001u, 0x9e3779b9u, 0x00000003u };

static const void *sl_iid_engine_var = sl_uuid_engine;
static const void *sl_iid_play_var   = sl_uuid_play;
static const void *sl_iid_bq_var     = sl_uuid_bq;

static int iid_is(const void *iid, const void *var_addr, const void *uuid)
{
    return iid == uuid || iid == var_addr;
}

/* --------------------------------------------------------- object model */
enum { K_ENGINE = 1, K_MIX = 2, K_PLAYER = 3 };

typedef struct {
    const void *vtbl;
    SLuint32    state;
    SLuint32    kind;
} sl_obj;

typedef struct {
    sl_obj      base;                 /* MUST be first */
    const void *play_cell;
    const void *bq_cell;
    SLuint32    play_state;
    SLuint32    bq_count, bq_index;
    void       *play_cb, *play_ctx;
    void       *bq_cb, *bq_ctx;
} sl_player;

static sl_obj g_engine;
static const void *engine_cell;       /* == engine_vtbl */

static int sl_trace_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("PB_TRACE_SL") ? 1 : 0;
    return on;
}
static unsigned sl_trace_left = 500;
#define TRACE_SL(...) do { \
        if (sl_trace_left && sl_trace_on()) { sl_trace_left--; pb_log(__VA_ARGS__); } \
    } while (0)

/* --------------------------------------------------- SLObjectItf methods */
static SLuint32 obj_Realize(void *self, SLuint32 async)
{
    (void)async;
    sl_obj *o = self;
    if (o) o->state = SL_OBJECT_STATE_REALIZED;
    TRACE_SL("[sl] Realize(kind=%u)\n", o ? o->kind : 0);
    return SL_RESULT_SUCCESS;
}

static SLuint32 obj_Resume(void *self, SLuint32 async)
{ (void)self; (void)async; return SL_RESULT_SUCCESS; }

static SLuint32 obj_GetState(void *self, SLuint32 *pState)
{
    if (pState) *pState = self ? ((sl_obj *)self)->state : SL_OBJECT_STATE_UNREALIZED;
    return SL_RESULT_SUCCESS;
}

static SLuint32 obj_GetInterface(void *self, const void *iid, void **out)
{
    sl_obj *o = self;
    if (!out) return SL_RESULT_PARAMETER_INVALID;
    if (o && o->kind == K_ENGINE && iid_is(iid, &sl_iid_engine_var, sl_uuid_engine)) {
        *out = (void *)&engine_cell;
        TRACE_SL("[sl] GetInterface(engine, ENGINE)\n");
        return SL_RESULT_SUCCESS;
    }
    if (o && o->kind == K_PLAYER) {
        sl_player *p = self;
        if (iid_is(iid, &sl_iid_play_var, sl_uuid_play)) {
            *out = (void *)&p->play_cell;
            TRACE_SL("[sl] GetInterface(player, PLAY)\n");
            return SL_RESULT_SUCCESS;
        }
        if (iid_is(iid, &sl_iid_bq_var, sl_uuid_bq)) {
            *out = (void *)&p->bq_cell;
            TRACE_SL("[sl] GetInterface(player, ANDROIDSIMPLEBUFFERQUEUE)\n");
            return SL_RESULT_SUCCESS;
        }
    }
    TRACE_SL("[sl] GetInterface(kind=%u iid=%p) -> unsupported\n", o ? o->kind : 0, iid);
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLuint32 obj_RegisterCallback(void *self, void *cb, void *ctx)
{ (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }

static void obj_AbortAsyncOperation(void *self) { (void)self; }

static SLuint32 obj_Destroy(void *self)
{
    sl_obj *o = self;
    if (o) o->state = 0;     /* leak: simpler than free() across guest threads */
    TRACE_SL("[sl] Destroy(kind=%u)\n", o ? o->kind : 0);
    return SL_RESULT_SUCCESS;
}

static SLuint32 obj_SetPriority(void *self, SLint32 prio, SLuint32 preempt)
{ (void)self; (void)prio; (void)preempt; return SL_RESULT_SUCCESS; }

static SLuint32 obj_GetPriority(void *self, SLint32 *prio, SLuint32 *preempt)
{
    (void)self;
    if (prio) *prio = 0;
    if (preempt) *preempt = 0;
    return SL_RESULT_SUCCESS;
}

static SLuint32 obj_SetLossOfControlInterfaces(void *self, int16_t n, void *ids, SLuint32 en)
{ (void)self; (void)n; (void)ids; (void)en; return SL_RESULT_SUCCESS; }

static const uintptr_t object_vtbl[10] = {
    (uintptr_t)obj_Realize,
    (uintptr_t)obj_Resume,
    (uintptr_t)obj_GetState,
    (uintptr_t)obj_GetInterface,
    (uintptr_t)obj_RegisterCallback,
    (uintptr_t)obj_AbortAsyncOperation,
    (uintptr_t)obj_Destroy,
    (uintptr_t)obj_SetPriority,
    (uintptr_t)obj_GetPriority,
    (uintptr_t)obj_SetLossOfControlInterfaces,
};

static const uintptr_t play_vtbl[14];   /* defined below (used by CreateAudioPlayer) */
static const uintptr_t bq_vtbl[4];

/* ---------------------------------------------------- SLEngineItf methods */
static SLuint32 eng_unsupported(void *self, void **out)
{
    (void)self;
    if (out) *out = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLuint32 eng_CreateOutputMix(void *self, void **pMix, SLuint32 n,
                                    const void *iids, const void *req)
{
    (void)self; (void)n; (void)iids; (void)req;
    if (!pMix) return SL_RESULT_PARAMETER_INVALID;
    sl_obj *m = calloc(1, sizeof *m);
    if (!m) return SL_RESULT_MEMORY_FAILURE;
    m->vtbl = object_vtbl;
    m->state = SL_OBJECT_STATE_UNREALIZED;
    m->kind = K_MIX;
    *pMix = m;
    TRACE_SL("[sl] CreateOutputMix -> %p\n", (void *)m);
    return SL_RESULT_SUCCESS;
}

static SLuint32 eng_CreateAudioPlayer(void *self, void **pPlayer, void *src,
                                      void *snk, SLuint32 n, const void *iids,
                                      const void *req)
{
    (void)self; (void)src; (void)snk; (void)n; (void)iids; (void)req;
    if (!pPlayer) return SL_RESULT_PARAMETER_INVALID;
    sl_player *p = calloc(1, sizeof *p);
    if (!p) return SL_RESULT_MEMORY_FAILURE;
    p->base.vtbl = object_vtbl;
    p->base.state = SL_OBJECT_STATE_UNREALIZED;
    p->base.kind = K_PLAYER;
    p->play_cell = play_vtbl;
    p->bq_cell = bq_vtbl;
    p->play_state = SL_PLAYSTATE_STOPPED;
    *pPlayer = &p->base;
    TRACE_SL("[sl] CreateAudioPlayer -> %p\n", (void *)&p->base);
    return SL_RESULT_SUCCESS;
}

static SLuint32 eng_QueryNumSupportedInterfaces(void *self, SLuint32 *n)
{
    (void)self;
    if (n) *n = 3;
    return SL_RESULT_SUCCESS;
}

static SLuint32 eng_QuerySupportedInterfaces(void *self, SLuint32 idx, void **iid)
{
    static const void *ids[3];
    ids[0] = sl_uuid_engine;
    ids[1] = sl_uuid_play;
    ids[2] = sl_uuid_bq;
    (void)self;
    if (iid && idx < 3) { *iid = (void *)ids[idx]; return SL_RESULT_SUCCESS; }
    return SL_RESULT_PARAMETER_INVALID;
}

static const uintptr_t engine_vtbl[13] = {
    (uintptr_t)eng_unsupported, /* CreateLEDDevice        */
    (uintptr_t)eng_unsupported, /* CreateVibraDevice      */
    (uintptr_t)eng_CreateAudioPlayer,
    (uintptr_t)eng_unsupported, /* CreateAudioRecorder    */
    (uintptr_t)eng_unsupported, /* CreateMidiPlayer       */
    (uintptr_t)eng_unsupported, /* CreateListener         */
    (uintptr_t)eng_unsupported, /* Create3DGroup          */
    (uintptr_t)eng_CreateOutputMix,
    (uintptr_t)eng_unsupported, /* CreateMetadataExtractor*/
    (uintptr_t)eng_unsupported, /* CreateExtensionObject  */
    (uintptr_t)eng_QueryNumSupportedInterfaces,
    (uintptr_t)eng_QuerySupportedInterfaces,
    (uintptr_t)eng_unsupported, /* QueryNumSupportedExtensions (1.1) */
};

/* ------------------------------------------------------ SLPlayItf methods */
#define PLAYER_OF_PLAY(cell) ((sl_player *)((char *)(cell) - offsetof(sl_player, play_cell)))
#define PLAYER_OF_BQ(cell)   ((sl_player *)((char *)(cell) - offsetof(sl_player, bq_cell)))

static SLuint32 play_SetPlayState(void *self, SLuint32 st)
{
    sl_player *p = PLAYER_OF_PLAY(self);
    p->play_state = st;
    TRACE_SL("[sl] SetPlayState(%u)\n", st);
    return SL_RESULT_SUCCESS;
}

static SLuint32 play_GetPlayState(void *self, SLuint32 *st)
{
    if (st) *st = PLAYER_OF_PLAY(self)->play_state;
    return SL_RESULT_SUCCESS;
}

static SLuint32 play_GetDuration(void *self, SLuint32 *d)
{
    (void)self;
    if (d) *d = SL_TIME_UNKNOWN;
    return SL_RESULT_SUCCESS;
}

static SLuint32 play_GetPosition(void *self, SLuint32 *p)
{
    (void)self;
    if (p) *p = 0;
    return SL_RESULT_SUCCESS;
}

static SLuint32 play_RegisterCallback(void *self, void *cb, void *ctx)
{
    sl_player *p = PLAYER_OF_PLAY(self);
    p->play_cb = cb;
    p->play_ctx = ctx;
    TRACE_SL("[sl] Play RegisterCallback cb=%p\n", cb);
    return SL_RESULT_SUCCESS;
}

static SLuint32 play_SetCallbackEventsMask(void *self, SLuint32 m)
{ (void)self; (void)m; return SL_RESULT_SUCCESS; }

static SLuint32 play_GetCallbackEventsMask(void *self, SLuint32 *m)
{ (void)self; if (m) *m = 0; return SL_RESULT_SUCCESS; }

static SLuint32 play_SetMarkerPosition(void *self, SLuint32 p)
{ (void)self; (void)p; return SL_RESULT_SUCCESS; }

static SLuint32 play_ClearMarkerPosition(void *self)
{ (void)self; return SL_RESULT_SUCCESS; }

static SLuint32 play_GetMarkerPosition(void *self, SLuint32 *p)
{ (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }

static SLuint32 play_SetPositionUpdatePeriod(void *self, SLuint32 ms)
{ (void)self; (void)ms; return SL_RESULT_SUCCESS; }

static SLuint32 play_GetPositionUpdatePeriod(void *self, SLuint32 *ms)
{ (void)self; if (ms) *ms = 1000; return SL_RESULT_SUCCESS; }

static SLuint32 play_SetPosition(void *self, SLuint32 pos)
{ (void)self; (void)pos; return SL_RESULT_SUCCESS; }

static const uintptr_t play_vtbl[14] = {
    (uintptr_t)play_SetPlayState,
    (uintptr_t)play_GetPlayState,
    (uintptr_t)play_GetDuration,
    (uintptr_t)play_GetPosition,
    (uintptr_t)play_RegisterCallback,
    (uintptr_t)play_SetCallbackEventsMask,
    (uintptr_t)play_GetCallbackEventsMask,
    (uintptr_t)play_SetMarkerPosition,
    (uintptr_t)play_ClearMarkerPosition,
    (uintptr_t)play_GetMarkerPosition,
    (uintptr_t)play_SetPositionUpdatePeriod,
    (uintptr_t)play_GetPositionUpdatePeriod,
    (uintptr_t)play_SetPosition,
    (uintptr_t)play_SetPosition,      /* pad: 1.1 has one more slot */
};

/* ------------------------------------- SLAndroidSimpleBufferQueueItf methods */
static SLuint32 bq_Enqueue(void *self, const void *buf, SLuint32 size)
{
    sl_player *p = PLAYER_OF_BQ(self);
    (void)buf;
    p->bq_count++;
    TRACE_SL("[sl] BQ Enqueue(%u bytes, queued=%u)\n", size, p->bq_count);
    return SL_RESULT_SUCCESS;
}

static SLuint32 bq_Clear(void *self)
{
    PLAYER_OF_BQ(self)->bq_count = 0;
    return SL_RESULT_SUCCESS;
}

static SLuint32 bq_GetState(void *self, SLuint32 *st)
{
    sl_player *p = PLAYER_OF_BQ(self);
    if (st) { st[0] = p->bq_count; st[1] = p->bq_index; }
    return SL_RESULT_SUCCESS;
}

static SLuint32 bq_RegisterCallback(void *self, void *cb, void *ctx)
{
    sl_player *p = PLAYER_OF_BQ(self);
    p->bq_cb = cb;
    p->bq_ctx = ctx;
    TRACE_SL("[sl] BQ RegisterCallback cb=%p ctx=%p\n", cb, ctx);
    return SL_RESULT_SUCCESS;
}

static const uintptr_t bq_vtbl[4] = {
    (uintptr_t)bq_Enqueue,
    (uintptr_t)bq_Clear,
    (uintptr_t)bq_GetState,
    (uintptr_t)bq_RegisterCallback,
};

/* ------------------------------------------------------------- slCreateEngine */
static SLuint32 sh_slCreateEngine(void **pEngine, SLuint32 nOpt,
                                  const void *opts, SLuint32 nIf,
                                  const void *iids, const void *req)
{
    (void)nOpt; (void)opts; (void)nIf; (void)iids; (void)req;
    if (!pEngine) return SL_RESULT_PARAMETER_INVALID;
    *pEngine = &g_engine;
    TRACE_SL("[sl] slCreateEngine -> %p\n", (void *)&g_engine);
    return SL_RESULT_SUCCESS;
}

/* ----------------------------------------------------------------- install */
void pb_shim_install_opensl(void)
{
    g_engine.vtbl = object_vtbl;
    g_engine.state = SL_OBJECT_STATE_REALIZED;
    g_engine.kind = K_ENGINE;
    engine_cell = engine_vtbl;

    pb_shim_register("slCreateEngine", (void *)sh_slCreateEngine, false);
    pb_shim_register("SL_IID_ENGINE", (void *)&sl_iid_engine_var, true);
    pb_shim_register("SL_IID_PLAY", (void *)&sl_iid_play_var, true);
    pb_shim_register("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (void *)&sl_iid_bq_var, true);
    pb_log("[opensl] minimal OpenSL ES object model installed (silent audio)\n");
}

/* The engine's SL_IID_* GOT slots (reloc-wired to the &vars above already;
 * force-written here so the double-deref can never see stale content). */
void pb_opensl_fixup_slots(void)
{
    if (!PB.base) return;
    *(uint64_t *)(PB.base + 0x38dc68) = (uint64_t)(uintptr_t)&sl_iid_engine_var;
    *(uint64_t *)(PB.base + 0x38dc70) = (uint64_t)(uintptr_t)&sl_iid_bq_var;
    *(uint64_t *)(PB.base + 0x38dc78) = (uint64_t)(uintptr_t)&sl_iid_play_var;
}
