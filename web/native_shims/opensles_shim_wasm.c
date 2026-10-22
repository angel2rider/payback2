/* opensles_shim_wasm.c — Payback 2 Web Port: OpenSL ES Web Audio Bridge.
 *
 * Implements OpenSL ES 1.0 object model for WASM, forwarding PCM buffers
 * to the browser's Web Audio API and pumping the audio feeder callback.
 */
#include "pb_wasm.h"
#include <stdlib.h>
#include <string.h>

typedef uint32_t SLuint32;
typedef int32_t  SLint32;

#define SL_RESULT_SUCCESS              0x00000000u
#define SL_OBJECT_STATE_REALIZED       0x00000002u
#define SL_PLAYSTATE_STOPPED           0x00000001u
#define SL_PLAYSTATE_PLAYING           0x00000003u

static const uint32_t sl_uuid_engine[4] = { 0x454e4731u, 0x00010001u, 0x9e3779b9u, 0x00000001u };
static const uint32_t sl_uuid_play[4]   = { 0x504c4159u, 0x00010001u, 0x9e3779b9u, 0x00000002u };
static const uint32_t sl_uuid_bq[4]     = { 0x42465131u, 0x00010001u, 0x9e3779b9u, 0x00000003u };

static const void *sl_iid_engine_var = sl_uuid_engine;
static const void *sl_iid_play_var   = sl_uuid_play;
static const void *sl_iid_bq_var     = sl_uuid_bq;

enum { K_ENGINE = 1, K_MIX = 2, K_PLAYER = 3 };

typedef struct {
    const void *vtbl;
    SLuint32    state;
    SLuint32    kind;
} sl_obj;

typedef struct {
    sl_obj      base;
    const void *play_cell;
    const void *bq_cell;
    SLuint32    play_state;
    void       (*bq_cb)(void *caller, void *pContext);
    void        *bq_ctx;
} sl_player;

static sl_obj g_engine;

static SLuint32 obj_Realize(void *self, SLuint32 async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLuint32 obj_Resume(void *self, SLuint32 async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLuint32 obj_GetState(void *self, SLuint32 *state)
{
    if (state) *state = SL_OBJECT_STATE_REALIZED;
    return SL_RESULT_SUCCESS;
}
static void obj_Destroy(void *self) { (void)self; }

static SLuint32 play_SetPlayState(void *self, SLuint32 state)
{
    sl_player *p = (sl_player *)((char *)self - offsetof(sl_player, play_cell));
    p->play_state = state;
    return SL_RESULT_SUCCESS;
}

static SLuint32 bq_Enqueue(void *self, const void *pBuffer, SLuint32 size)
{
    sl_player *p = (sl_player *)((char *)self - offsetof(sl_player, bq_cell));
#ifdef __EMSCRIPTEN__
    js_audio_enqueue_pcm((const int16_t *)pBuffer, size / 2);
#endif
    // Trigger feeder callback so engine queues the next audio block
    if (p->bq_cb) {
        p->bq_cb((void *)&p->bq_cell, p->bq_ctx);
    }
    return SL_RESULT_SUCCESS;
}

static SLuint32 bq_Clear(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLuint32 bq_RegisterCallback(void *self, void (*callback)(void *, void *), void *pContext)
{
    sl_player *p = (sl_player *)((char *)self - offsetof(sl_player, bq_cell));
    p->bq_cb = callback;
    p->bq_ctx = pContext;
    return SL_RESULT_SUCCESS;
}

static const void *play_vtbl[] = { (void *)play_SetPlayState };
static const void *bq_vtbl[]   = { (void *)bq_Enqueue, (void *)bq_Clear, NULL, (void *)bq_RegisterCallback };

static SLuint32 obj_GetInterface(void *self, const void *pInterfaceId, void *pInterface)
{
    sl_obj *o = (sl_obj *)self;
    if (o->kind == K_PLAYER) {
        sl_player *p = (sl_player *)self;
        if (pInterfaceId == sl_iid_play_var || pInterfaceId == sl_uuid_play) {
            *(const void ***)pInterface = (const void **)&p->play_cell;
            return SL_RESULT_SUCCESS;
        }
        if (pInterfaceId == sl_iid_bq_var || pInterfaceId == sl_uuid_bq) {
            *(const void ***)pInterface = (const void **)&p->bq_cell;
            return SL_RESULT_SUCCESS;
        }
    }
    return SL_RESULT_SUCCESS;
}

static const void *obj_vtbl[] = {
    (void *)obj_Realize, (void *)obj_Resume, (void *)obj_GetState, (void *)obj_GetInterface,
    NULL, NULL, (void *)obj_Destroy
};

static SLuint32 engine_CreateAudioPlayer(void *self, void **ppPlayer, void *pAudioSrc, void *pAudioSnk,
                                         SLuint32 numInterfaces, const void **pInterfaceIds, const SLuint32 *pInterfaceRequired)
{
    (void)self; (void)pAudioSrc; (void)pAudioSnk; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
    sl_player *p = (sl_player *)calloc(1, sizeof(sl_player));
    p->base.vtbl = obj_vtbl;
    p->base.kind = K_PLAYER;
    p->base.state = SL_OBJECT_STATE_REALIZED;
    p->play_cell = play_vtbl;
    p->bq_cell = bq_vtbl;
    *ppPlayer = p;
    return SL_RESULT_SUCCESS;
}

static SLuint32 engine_CreateOutputMix(void *self, void **ppMix, SLuint32 numInterfaces,
                                       const void **pInterfaceIds, const SLuint32 *pInterfaceRequired)
{
    (void)self; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
    sl_obj *m = (sl_obj *)calloc(1, sizeof(sl_obj));
    m->vtbl = obj_vtbl;
    m->kind = K_MIX;
    m->state = SL_OBJECT_STATE_REALIZED;
    *ppMix = m;
    return SL_RESULT_SUCCESS;
}

static const void *engine_vtbl[] = {
    NULL, NULL, (void *)engine_CreateAudioPlayer, NULL, NULL, NULL, (void *)engine_CreateOutputMix
};
static const void *g_engine_cell = engine_vtbl;

static SLuint32 sl_CreateEngine(void **ppEngine, SLuint32 numOptions, const void *pEngineOptions,
                                SLuint32 numInterfaces, const void **pInterfaceIds, const SLuint32 *pInterfaceRequired)
{
    (void)numOptions; (void)pEngineOptions; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
    g_engine.vtbl = obj_vtbl;
    g_engine.kind = K_ENGINE;
    g_engine.state = SL_OBJECT_STATE_REALIZED;
    *ppEngine = &g_engine;
    return SL_RESULT_SUCCESS;
}

void pb_shim_install_opensl(void)
{
    pb_shim_register("slCreateEngine", (void *)sl_CreateEngine, false);
    pb_shim_register("SL_IID_ENGINE",  (void *)&sl_iid_engine_var, true);
    pb_shim_register("SL_IID_PLAY",    (void *)&sl_iid_play_var, true);
    pb_shim_register("SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (void *)&sl_iid_bq_var, true);
    pb_shim_register("SL_IID_BUFFERQUEUE",              (void *)&sl_iid_bq_var, true);
}
