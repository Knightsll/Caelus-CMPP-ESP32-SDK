#!/usr/bin/env python3
"""Compile the production settings transaction with deterministic NVS stubs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/voice_media.c").read_text()
begin = source.index("esp_err_t voice_media_set_barge_threshold(")
end = source.index("\nesp_err_t voice_media_stop(", begin)
function = source[begin:end]
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef int esp_err_t;
typedef int nvs_handle_t;
enum {ESP_OK,ESP_ERR_INVALID_ARG,ESP_ERR_INVALID_STATE,ESP_FAIL,NVS_READWRITE};
typedef struct {
    bool recording,output_busy,awaiting_response,cue_busy,barge_threshold_saved;
    struct {bool active;} conversation;
    struct {bool awaiting_stop;} wire;
    unsigned barge_min_rms;
    void *wake;
} voice_media_t;
static int locked,writes,commits,closes,commit_failure;
static unsigned saved,applied;
static void take(voice_media_t *b) {(void)b; assert(!locked);locked=1;}
static void give(voice_media_t *b) {(void)b; assert(locked);locked=0;}
static int nvs_open(const char *key,int mode,int *h) {assert(!strcmp(key,"mcp_light") && mode==NVS_READWRITE);*h=1;return ESP_OK;}
static int nvs_set_u16(int h,const char *key,uint16_t value) {assert(h==1 && !strcmp(key,"barge_rms"));++writes;saved=value;return ESP_OK;}
static int nvs_commit(int h) {assert(h==1);++commits;return commit_failure ? ESP_FAIL : ESP_OK;}
static void nvs_close(int h) {assert(h==1);++closes;}
static void voice_wake_set_barge_min_rms(void *w,unsigned rms) {(void)w;applied=rms;}
'''
test = r'''
int main(void) {
    voice_media_t b={.barge_min_rms=1000}; bool changed=true;
    assert(voice_media_set_barge_threshold(&b,79,false,&changed)==ESP_ERR_INVALID_ARG && !locked);
    assert(voice_media_set_barge_threshold(&b,8001,false,&changed)==ESP_ERR_INVALID_ARG && !locked);
    assert(voice_media_set_barge_threshold(&b,1200,true,&changed)==ESP_OK && !changed && !writes);
    bool *busy[]={&b.recording,&b.output_busy,&b.awaiting_response,&b.cue_busy,&b.conversation.active,&b.wire.awaiting_stop};
    for(unsigned i=0;i<6;i++) {
        *busy[i]=true;
        assert(voice_media_set_barge_threshold(&b,1200,false,&changed)==ESP_ERR_INVALID_STATE && !changed && !writes && !locked);
        *busy[i]=false;
    }
    commit_failure=1;
    assert(voice_media_set_barge_threshold(&b,1200,false,&changed)==ESP_FAIL);
    assert(!changed && b.barge_min_rms==1000 && !applied && !locked && closes==1);
    commit_failure=0;
    assert(voice_media_set_barge_threshold(&b,1200,false,&changed)==ESP_OK);
    assert(changed && saved==1200 && applied==1200 && b.barge_min_rms==1200 && b.barge_threshold_saved && !locked);
    int count=commits;
    assert(voice_media_set_barge_threshold(&b,1200,false,&changed)==ESP_OK && !changed && commits==count);
    assert(voice_media_set_barge_threshold(&b,8000,false,&changed)==ESP_OK && applied==8000);
    assert(voice_media_set_barge_threshold(&b,80,false,&changed)==ESP_OK && applied==80);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="caelus-barge-settings-") as tmp:
    p = Path(tmp)
    (p / "test.c").write_text(stubs + function + test)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                    str(p / "test.c"), "-o", str(p / "test")], check=True)
    subprocess.run([str(p / "test")], check=True)
print("Barge settings: range, dry-run, six busy states, commit failure and idempotent save passed")
