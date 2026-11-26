#!/usr/bin/env python3
"""Exercise the real AEC frame bridge with a deterministic fake engine.

Checks aliasing, ordering, bounded FIFO and reference gating; does not certify
the ESP-SR algorithm or the physical MIC3 wiring.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADERS = {
    "esp_err.h": "typedef int esp_err_t;\n",
    "esp_log.h": '#define ESP_LOGI(...) ((void)0)\n',
    "esp_timer.h": '#include <stdint.h>\nstatic inline int64_t esp_timer_get_time(void) { static int64_t t; return ++t; }\n',
    "freertos/FreeRTOS.h": "typedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n#define portENTER_CRITICAL(p) ((void)(p))\n#define portEXIT_CRITICAL(p) ((void)(p))\n",
    "esp_heap_caps.h": """#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void *heap_caps_aligned_calloc(size_t a,size_t n,size_t s,int c) { (void)a; (void)c; return calloc(n,s); }
static inline void *heap_caps_calloc(size_t n,size_t s,int c) { (void)c; return calloc(n,s); }
""",
    "esp_aec.h": """#include <stdint.h>
typedef struct {int mic_num,ref_num,out_num,filter_length,sample_rate,caps,mode,nlp_level;} aec_config_t;
typedef struct {int n;} aec_handle_t;
#define AEC_MODE_FD_LOW_COST 5
#define AEC_NLP_LEVEL_AGGR 2
aec_handle_t *aec_create_from_config(aec_config_t *c);
int aec_get_chunksize(aec_handle_t *h);
void aec_process(aec_handle_t *h,int16_t *m,int16_t *r,int16_t *o);
void aec_destroy(aec_handle_t *h);
""",
}
TEST = r'''
#include <assert.h>
#include <stdlib.h>
#include "voice_echo.h"
#include "esp_aec.h"
static aec_handle_t handle;
static int suppress;
aec_handle_t *aec_create_from_config(aec_config_t *c) { assert(c->sample_rate==16000); return &handle; }
int aec_get_chunksize(aec_handle_t *h) { return h->n; }
void aec_destroy(aec_handle_t *h) { (void)h; }
void aec_process(aec_handle_t *h,int16_t *m,int16_t *r,int16_t *o) {
    for(int i=0;i<h->n;i++) { if(!suppress) assert(r[i]==-m[i]); o[i]=suppress == 2 ? m[i]/4 : suppress ? 0 : m[i]; }
}
int main(int argc,char **argv) {
    assert(argc==2); handle.n=atoi(argv[1]); assert(voice_echo_init());
    int16_t m[320],r[320];
    for(int f=0;f<200;f++) {
        for(int i=0;i<320;i++) { m[i]=(f*320+i)%6000+1; r[i]=-m[i]; }
        assert(voice_echo_process(m,r,m)); /* in-place, same as production */
        for(int i=0;i<320;i++) { int t=f*320+i-handle.n; assert(m[i]==(t<0 ? 0 : t%6000+1)); }
    }
    assert(!voice_echo_stats().ready);
    suppress=1;
    for(int f=0;f<50;f++) {
        for(int i=0;i<320;i++) { m[i]=2000; r[i]=0; }
        assert(voice_echo_process(m,r,m));
    }
    assert(!voice_echo_stats().ready); /* suppression alone is insufficient */
    /* Too little reference must not qualify, even with suppression. */
    for(int f=0;f<150;f++) {
        for(int i=0;i<320;i++) { m[i]=2000; r[i]=(f%50<2) ? 1000 : 0; }
        assert(voice_echo_process(m,r,m));
    }
    assert(!voice_echo_stats().ready);
    /* Pulsed syllables: 100ms audio, 200ms pause, never 500ms contiguous.
     * A quarter-amplitude output has genuine attenuation on reference frames. */
    suppress=2;
    for(int f=0;f<150;f++) {
        for(int i=0;i<320;i++) { m[i]=(f%15<5) ? 2400 : 0; r[i]=m[i]/2; }
        assert(voice_echo_process(m,r,m));
    }
    assert(voice_echo_stats().ready);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="caelus-echo-test-") as tmp:
    directory = Path(tmp)
    for name, content in HEADERS.items():
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    source = directory / "test.c"
    source.write_text(TEST)
    exe = directory / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                    "-I", str(directory), "-I", str(ROOT / "main"), str(source),
                    str(ROOT / "main/voice_echo.c"), "-lm", "-o", str(exe)], check=True)
    for size in (160, 256, 320, 480, 512, 1024):
        subprocess.run([str(exe), str(size)], check=True)
print("AEC bridge: six frame sizes, in-place PCM, ordering and reference gate passed")
