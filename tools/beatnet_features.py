"""The BeatNet+ feature front end, built from madmom exactly as BeatNet+ builds it.

This is the reference that src/core/features/ has to match (HANDOFF §5.2, §8 Phase 2).
It is BeatNet+'s ``LOG_SPECT`` class (src/BeatNetPlus/log_spect.py at commit
bb90eb0a9065b101a4b4c4cb2b2061950266cb4b) restated without the torch dependency; the
processor chain and every parameter are copied from there, not from the handoff, whose
description of the log step ("log1p") is wrong: madmom's
LogarithmicSpectrogramProcessor(mul=1, add=1) computes log10(1 + x).

What the chain does, for a mono float signal at 22050 Hz:

- frames of 1764 samples every 441 (80 ms / 20 ms), frame k centred on sample 441 k,
  zero-padded at the edges, ceil(N / 441) frames in all;
- a symmetric numpy.hanning(1764) window, unscaled for float input;
- a 1764-point FFT keeping bins 0..881 (Nyquist dropped), magnitudes;
- madmom's LogarithmicFilterbank: 24 bands per octave between 30 Hz and 17000 Hz,
  reference 440 Hz, each filter normalised to sum 1, duplicates removed — 144 bands;
- log10(1 + x);
- the positive first difference to the previous frame (diff_ratio 0.5 works out to a
  lag of one frame for this window and hop), zero for the first frame;
- the log bands and the differences stacked side by side: 288 values per frame.

One deliberate difference: LOG_SPECT.process_audio ends with ``feats.T``, handing torch
(288, frames). Nothing here transposes, so a run is (frames, 288) — the order the C++
side produces and the goldens are stored in. The numbers are the same numbers.
"""

import numpy as np
from madmom.audio.signal import FramedSignalProcessor, SignalProcessor
from madmom.audio.spectrogram import (
    FilteredSpectrogramProcessor,
    LogarithmicSpectrogramProcessor,
    SpectrogramDifferenceProcessor,
)
from madmom.audio.stft import ShortTimeFourierTransformProcessor
from madmom.processors import ParallelProcessor, SequentialProcessor

SAMPLE_RATE = 22050
FRAME_SIZE = 1764  # 80 ms
HOP_SIZE = 441  # 20 ms
NUM_BINS = FRAME_SIZE // 2  # 882: madmom keeps DC up to, not including, Nyquist
NUM_BANDS = 144
FEATURE_DIM = 2 * NUM_BANDS


class FeaturePipeline:
    """BeatNet+'s LOG_SPECT(sample_rate=22050, win_length=1764, hop_size=441, n_bands=[24])
    in its 'online' mode: the whole signal at once, every frame."""

    def __init__(self):
        # The lines below are LOG_SPECT.__init__ with its loop over one frame size
        # unrolled. SignalProcessor ignores win_length; BeatNet+ passes it anyway.
        sig = SignalProcessor(num_channels=1, win_length=FRAME_SIZE, sample_rate=SAMPLE_RATE)
        frames = FramedSignalProcessor(frame_size=FRAME_SIZE, hop_size=HOP_SIZE)
        stft = ShortTimeFourierTransformProcessor()
        self.filt = FilteredSpectrogramProcessor(num_bands=24, fmin=30, fmax=17000, norm_filters=True)
        spec = LogarithmicSpectrogramProcessor(mul=1, add=1)
        diff = SpectrogramDifferenceProcessor(diff_ratio=0.5, positive_diffs=True, stack_diffs=np.hstack)
        multi = ParallelProcessor([])
        multi.append(SequentialProcessor((frames, stft, self.filt, spec, diff)))
        self.pipe = SequentialProcessor((sig, multi, np.hstack))

    def features(self, audio):
        """(num_frames, 288) float32 for a mono float32 signal at 22050 Hz."""
        audio = np.asarray(audio, dtype=np.float32)
        if audio.ndim != 1:
            raise ValueError(f"expected a mono signal, got shape {audio.shape}")
        feats = self.pipe(audio)
        expected = (int(np.ceil(len(audio) / HOP_SIZE)), FEATURE_DIM)
        if feats.shape != expected or feats.dtype != np.float32:
            raise RuntimeError(f"madmom returned {feats.dtype}{feats.shape}, expected float32{expected}")
        return feats

    def filterbank(self):
        """The (882, 144) float32 matrix madmom applied, available after features() ran once."""
        fb = self.filt.filterbank
        if not isinstance(fb, np.ndarray):
            raise RuntimeError("the filterbank is only instantiated by a run; call features() first")
        fb = np.asarray(fb)
        if fb.shape != (NUM_BINS, NUM_BANDS) or fb.dtype != np.float32:
            raise RuntimeError(f"unexpected filterbank {fb.dtype}{fb.shape}")
        return fb
