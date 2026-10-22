/* main_wasm.c — Payback 2 Web Port: WASM Entry & Export Bridge.
 */
#include "pb_wasm.h"
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE

EM_JS(int, js_asset_open, (const char *path), {
  if (!window.g_engine_bridge) return 0;
  return window.g_engine_bridge.openAsset(UTF8ToString(path));
});

EM_JS(int, js_asset_read, (int handle, void *buf, int n), {
  if (!window.g_engine_bridge) return -1;
  const dest = HEAPU8.subarray(buf, buf + n);
  return window.g_engine_bridge.readAsset(handle, dest, n);
});

EM_JS(int, js_asset_length, (int handle), {
  if (!window.g_engine_bridge) return 0;
  return window.g_engine_bridge.getAssetLength(handle);
});

EM_JS(void, js_asset_close, (int handle), {
  if (window.g_engine_bridge) {
    window.g_engine_bridge.closeAsset(handle);
  }
});

EM_JS(void, js_audio_enqueue_pcm, (const int16_t *buf, int n_samples), {
  if (!window.g_engine_bridge) return;
  const pcm16 = HEAP16.subarray(buf >> 1, (buf >> 1) + n_samples);
  window.g_engine_bridge.enqueuePcmAudio(pcm16);
});

#else
#define EXPORT
#endif

EXPORT
int pb_wasm_init(void)
{
    printf("[pb_wasm] Initializing Payback 2 WebAssembly Shims...\n");
    pb_jni_init();
    pb_shim_install_android();
    pb_shim_install_opensl();
    printf("[pb_wasm] Shims successfully initialized!\n");
    return 0;
}

EXPORT
void *pb_wasm_lookup_symbol(const char *name)
{
    bool is_data = false;
    return pb_shim_lookup(name, &is_data);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    return pb_wasm_init();
}
