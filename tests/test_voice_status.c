/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "voice_status.h"

int main(void)
{
    unsigned ready = VOICE_LED_NETWORK | VOICE_LED_READY;
    for (unsigned ms = 0; ms < 4000; ++ms) {
        assert(voice_status_lit(VOICE_LED_SETUP, ms) == (ms % 1000 < 500));
        assert(!voice_status_lit(VOICE_LED_NETWORK, ms)); /* Paired != ready. */
        assert(voice_status_lit(ready, ms) == (ms % 2000 < 100));
        assert(voice_status_lit(ready | VOICE_LED_RECORDING, ms));
        assert(voice_status_lit(ready | VOICE_LED_PLAYING, ms) == (ms % 500 < 250));
        assert(voice_status_lit(ready | VOICE_LED_WAITING, ms) == (ms % 3000 < 1500));
        assert(voice_status_lit(ready | VOICE_LED_WAITING | VOICE_LED_PLAYING, ms) == (ms % 500 < 250));
        assert(voice_status_lit(ready | VOICE_LED_RECORDING | VOICE_LED_PLAYING, ms));
        /* Disconnect/setup/error must override stale media flags. */
        assert(voice_status_lit(VOICE_LED_MEDIA, ms) == voice_status_lit(0, ms));
        assert(voice_status_lit(VOICE_LED_SETUP | ready | VOICE_LED_RECORDING, ms)
               == voice_status_lit(VOICE_LED_SETUP, ms));
        assert(voice_status_lit(VOICE_LED_FAULT | ready | VOICE_LED_RECORDING, ms)
               == (ms % 200 < 100));
        assert(voice_status_lit(VOICE_LED_MAINTENANCE | VOICE_LED_FAULT | VOICE_LED_SETUP, ms));
    }
    puts("voice status: state priority and timing passed");
    return 0;
}
