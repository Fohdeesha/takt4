"""Independent pulse evidence from the audio itself, to referee sections where the
reference trackers disagree: per window, the strongest periodicities of the kick-band
(30-120 Hz) onset envelope and of the full-band envelope, as BPM. No model involved.

    python tools/refeval/tempogram.py 09_Defang [window seconds]
"""
import sys

import numpy as np
import soundfile as sf

from common import AUDIO


def onset_envelope(y, sr, lo, hi, hop=220, n_fft=1024):
    frames = 1 + (len(y) - n_fft) // hop
    win = np.hanning(n_fft)
    freqs = np.fft.rfftfreq(n_fft, 1 / sr)
    band = (freqs >= lo) & (freqs < hi)
    env = np.zeros(frames)
    prev = None
    for i in range(frames):
        seg = y[i * hop:i * hop + n_fft] * win
        mag = np.log1p(10 * np.abs(np.fft.rfft(seg))[band])
        if prev is not None:
            env[i] = np.maximum(mag - prev, 0).sum()
        prev = mag
    return env, sr / hop


def peaks_bpm(env, fps, lo_bpm=50, hi_bpm=240, top=3):
    env = env - env.mean()
    if not np.any(env):
        return []
    ac = np.correlate(env, env, mode="full")[len(env) - 1:]
    ac /= ac[0] + 1e-9
    lags = np.arange(len(ac))
    lo, hi = int(fps * 60 / hi_bpm), int(fps * 60 / lo_bpm)
    seg = ac[lo:hi]
    idx = [i for i in range(1, len(seg) - 1) if seg[i] > seg[i - 1] and seg[i] >= seg[i + 1]]
    idx.sort(key=lambda i: -seg[i])
    return [(60 * fps / (lags[lo + i]), seg[i]) for i in idx[:top]]


def main(argv):
    stem = argv[0]
    win = float(argv[1]) if len(argv) > 1 else 10.0
    y, sr = sf.read(str(AUDIO / f"{stem}.wav"), dtype="float32")
    kick, fps = onset_envelope(y, sr, 30, 120)
    full, _ = onset_envelope(y, sr, 30, 8000)
    print(f"{stem}: kick-band and full-band autocorrelation peaks per {win:.0f} s window "
          f"(BPM, strength)")
    for lo in np.arange(0, len(y) / sr, win):
        a, b = int(lo * fps), int((lo + win) * fps)
        k = peaks_bpm(kick[a:b], fps)
        f = peaks_bpm(full[a:b], fps)
        ks = "  ".join(f"{bpm:5.1f}({s:.2f})" for bpm, s in k)
        fs = "  ".join(f"{bpm:5.1f}({s:.2f})" for bpm, s in f)
        print(f"{lo:5.0f}  kick: {ks:<45}  full: {fs}")


if __name__ == "__main__":
    main(sys.argv[1:])
