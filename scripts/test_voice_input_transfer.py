#!/usr/bin/env python3
"""Exercise the actual capture transition: button cancellation, start, PCM, end."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/voice_media.c").read_text()
begin = source.index("        if (b->wire.ready && !blocked && err == ESP_OK)")
end = source.index("        bool hello_expired", begin)
transition = source[begin:end]
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum { ESP_OK, VOICE_STOP_AUDIO_STOP_FAILED, VOICE_PTT_NONE, VOICE_PTT_START, VOICE_PTT_END };
#define VOICE_FRAME_SAMPLES 320
#define VOICE_FRAME_BYTES 640
#define WAKE_PREROLL_FRAMES 36
#define CONFIG_CMPP_VOICE_FOLLOWUP_TIMEOUT_MS 15000
#define VOICE_LED_PLAYING 1
#define VOICE_LED_WAITING 2
#define VOICE_LED_RECORDING 4
#define ESP_LOGI(...) ((void)0)
typedef struct {
    bool recording, playing, output_busy, awaiting_response;
    unsigned input_turn, playback_generation;
    struct {bool ready, interrupt_enabled;} wire;
    struct {bool armed;} ptt;
    struct {bool active;} conversation;
    void *playback;
} board;
static unsigned stage, discarded, reset, starts, ends, count;
static int samples[100];
static int64_t now_ms(void) {return 100;}
static void xQueueReset(void *q) {(void)q; ++reset;}
static int voice_audio_discard_output(void) {++discarded; stage=1; return ESP_OK;}
static void voice_status_update(int clear,int set) {(void)clear;(void)set;}
static bool voice_wire_begin_input(void *w,const char *id) {(void)w;assert(stage==1);assert(!strcmp(id,"in_1"));stage=2;return true;}
static void voice_wire_end_input(void *w) {(void)w;}
static bool input_event(board *b,const char *event) {
    (void)b;
    if (!strcmp(event,"input.start")) {assert(stage==2);++starts;stage=3;}
    else {assert(!strcmp(event,"input.end") && count>0);++ends;}
    return true;
}
static bool voice_conversation_step(void *s,bool ready,bool busy,bool started,int64_t now,unsigned ms) {
    (void)busy;(void)now;assert(ready && started && ms==15000);*(bool *)s=true;return true;
}
static bool upload_locked(board *b,bool control,const void *data,unsigned bytes,const char *tag) {
    (void)b;assert(stage==3 && !control && bytes==640 && !strcmp(tag,"pcm"));
    samples[count++]=*(const int16_t *)data;return true;
}
static void step(board *b,int ptt,int sample) {
    bool blocked=false,ok=true;
    int err=ESP_OK,input_failure=0;
    int16_t pcm[320]={sample},previous_pcm[320]={-1};
'''
checks = r'''
    assert(ok && !input_failure);
}
int main(void) {
    board b={.wire={true,true},.playing=true,.output_busy=true,.playback_generation=7};
    for(int i=0;i<100;i++) step(&b,VOICE_PTT_NONE,14);
    assert(b.playing && b.output_busy && !discarded && !starts && !count);
    step(&b,VOICE_PTT_START,14);
    assert(discarded==1 && reset==1 && starts==1 && count==2);
    assert(samples[0]==-1 && samples[1]==14);
    assert(b.playback_generation==8 && b.recording && b.conversation.active && !b.playing);
    step(&b,VOICE_PTT_NONE,15);
    step(&b,VOICE_PTT_END,16);
    assert(count==4 && samples[2]==15 && samples[3]==16 && ends==1 && starts==1);
    assert(!b.recording && b.awaiting_response);
    for (int i=0;i<100;i++) step(&b,VOICE_PTT_NONE,17);
    assert(starts==1 && ends==1 && count==4);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="caelus-input-transfer-") as tmp:
    p = Path(tmp)
    (p / "test.c").write_text(stubs + transition + checks)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                    str(p / "test.c"), "-o", str(p / "test")], check=True)
    subprocess.run([str(p / "test")], check=True)
print("Input transfer: no press preserves playback; press cancels once, start precedes debounce PCM, tail precedes end")
