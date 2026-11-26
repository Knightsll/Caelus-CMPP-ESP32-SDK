#!/usr/bin/env python3
"""Compare the firmware's actual C frontend/config against training's Python frontend."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import numpy as np
from wake_features import features

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "managed_components/esphome__esp-micro-speech-features"
SOURCE = r'''
#include "voice_micro_frontend.h"
int extract(const int16_t *pcm, size_t count, float *out, size_t capacity) {
    struct FrontendState state = {0};
    struct FrontendConfig config = voice_micro_frontend_config();
    if (!FrontendPopulateState(&config, &state, 16000)) return -1;
    size_t written = 0;
    while (count) {
        size_t used = 0;
        struct FrontendOutput f = FrontendProcessSamples(&state, pcm, count, &used);
        if (!used || written + f.size > capacity) { written = 0; break; }
        pcm += used; count -= used;
        for (size_t i = 0; i < f.size; ++i) out[written++] = f.values[i] / 25.6f;
    }
    FrontendFreeStateContents(&state);
    return written;
}
'''

def main():
    rng = np.random.default_rng(20261124)
    sample = np.arange(16000)
    cases = [np.zeros(32000), 0.3 * np.sin(sample * 2 * np.pi * 400 / 16000), rng.normal(0, 0.04, 64000)]
    with tempfile.TemporaryDirectory(prefix="caelus-frontend-") as folder:
        directory = Path(folder)
        wrapper = directory / "check.c"
        wrapper.write_text(SOURCE)
        library = directory / "frontend.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-I", str(ROOT / "main"), "-I", str(COMPONENT / "include"), "-I", str(COMPONENT / "src"), str(wrapper), *map(str, sorted((COMPONENT / "src").glob("*.c"))), "-lm", "-o", str(library)], check=True)
        lib = ctypes.CDLL(str(library))
        lib.extract.argtypes = [ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t, ctypes.POINTER(ctypes.c_float), ctypes.c_size_t]
        lib.extract.restype = ctypes.c_int
        for audio in cases:
            audio = audio.astype(np.float32)
            pcm = np.clip(audio * 32768, -32768, 32767).astype(np.int16)
            expected = features(audio)
            actual = np.zeros(len(audio) // 160 * 40, dtype=np.float32)
            size = lib.extract(pcm.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)), len(pcm), actual.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), len(actual))
            assert size > 0
            actual = actual[:size].reshape(-1, 40)
            np.testing.assert_allclose(actual, expected, rtol=0, atol=1e-5)
        print("PASS: identical firmware/training features for silence, tone and noise; 16 kHz / 30 ms / 10 ms / 40 bins")

if __name__ == "__main__":
    main()
