#!/usr/bin/env python3
"""Run the production playback transfer function against a fake TX driver.

Extract only this platform-independent function, so ESP-IDF HTTP/audio setup
doesn't have to be stubbed. Test partial writes, yielding, cancellation and
bounded hardware failure without changing its implementation.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/voice_media.c").read_text()
begin = source.index("static esp_err_t playback_write_locked(")
end = source.index("\nstatic void playback_task(", begin)
function = source[begin:end]
begin_queue = source.index("static bool queue_playback_locked(")
end_queue = source.index("\nstatic esp_err_t websocket_handler(", begin_queue)
queue_function = source[begin_queue:end_queue]
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_TIMEOUT, ESP_FAIL };
typedef struct { bool ready, blocked; } wire_t;
typedef struct { uint32_t generation; } packet_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
typedef struct { void *playback; struct { bool active; } lifecycle; uint32_t playback_generation; wire_t wire; } voice_media_t;
static voice_media_t board;
static int locked, yields, calls, cancellation, failure, queue_result;
static size_t accepted;
static int64_t clock_ms;
static int64_t now_ms(void) { return clock_ms; }
static bool voice_wire_output_blocked(wire_t *w) { return w->blocked; }
static void give(voice_media_t *b) { assert(b==&board && locked); locked=0; }
static void take(voice_media_t *b) { assert(b==&board && !locked); locked=1; }
static void vTaskDelay(int ticks) {
    assert(!locked && ticks==1); ++yields; clock_ms+=10;
    if(cancellation==1) ++board.playback_generation;
    if(cancellation==2) board.lifecycle.active=false;
    if(cancellation==3) board.wire.blocked=true;
}
static esp_err_t voice_audio_try_write(const int16_t *pcm,size_t samples,size_t *written) {
    assert(locked); ++calls;
    if(failure==1) return ESP_FAIL;
    *written = failure==2 || calls%2==0 ? 0 : samples<80 ? samples : 80;
    for(size_t i=0;i<*written;i++) assert(pcm[i]==(int16_t)(accepted+i));
    accepted+=*written;
    return ESP_OK;
}
static int xQueueSend(void *q,const packet_t *p,int timeout) {
    (void)q; assert(!locked && p->generation==1 && timeout==250);
    vTaskDelay(1); return queue_result;
}
static void reset(void) {
    board=(voice_media_t){.lifecycle={true},.playback_generation=1,.wire={true,false}};
    queue_result=pdTRUE; locked=1; yields=calls=cancellation=failure=0; accepted=0; clock_ms=0;
}
'''
tests = r'''
int main(void) {
    int16_t pcm[320]; for(int i=0;i<320;i++) pcm[i]=i;
    bool valid=true;
    reset();
    assert(playback_write_locked(&board,1,pcm,320,&valid)==ESP_OK);
    assert(valid && accepted==320 && yields>0 && locked);
    for(int mode=1;mode<=3;mode++) {
        reset(); cancellation=mode; valid=true;
        assert(playback_write_locked(&board,1,pcm,320,&valid)==ESP_OK);
        assert(!valid && accepted==80 && calls==1 && locked);
    }
    reset(); failure=1; valid=true;
    assert(playback_write_locked(&board,1,pcm,320,&valid)==ESP_FAIL);
    assert(valid && accepted==0 && locked);
    reset(); failure=2; valid=true;
    assert(playback_write_locked(&board,1,pcm,320,&valid)==ESP_ERR_TIMEOUT);
    assert(valid && accepted==0 && yields==25 && locked);
    packet_t packet={.generation=1};
    reset(); assert(queue_playback_locked(&board,&packet)); assert(locked && yields==1);
    reset(); queue_result=0; assert(!queue_playback_locked(&board,&packet)); assert(locked);
    for(int mode=1;mode<=3;mode++) {
        reset(); cancellation=mode; queue_result=0;
        assert(queue_playback_locked(&board,&packet)); assert(locked);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="caelus-tx-test-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(stubs + function + queue_function + tests)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("Playback transfer: partial/zero writes, lock yielding, stale generation/stop barrier timeout and bounded queue backpressure passed")
