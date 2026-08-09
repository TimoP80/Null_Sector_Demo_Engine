#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# NULL SECTOR // NEURAL DUST - procedural soundtrack generator.
#
# Synthesizes the full 4:32 electronic track (128 BPM, 145 bars, 9 sections)
# with numpy and writes 48 kHz stereo WAV. The arrangement is tuned to the
# engine's audio analyser so the visuals react the way the production needs:
#   bass band (<750 Hz)   <- kick drum + sub/saw bass   (drives uBass / uKick)
#   mid band  (~1800 Hz)  <- pads, arps, snares         (drives uMid)
#   treble    (>3200 Hz)  <- hats, risers, shimmer      (drives uHigh)
#   onset                <- snare/clap hits, impacts   (drives uOnset)
#
# All DSP is vectorized (FFT convolution FIR filters) - no per-sample loops.
#
# Run:  python tools/gen_neural_dust_track.py
# Writes: data/neural_dust/track.wav
# ---------------------------------------------------------------------------
import os
import numpy as np
import wave

SR = 48000
BPM = 128.0
BEAT = 60.0 / BPM          # 0.46875 s
BAR = BEAT * 4.0           # 1.875 s

# --- section map (start bar, length in bars) ---------------------------------
SECTIONS = [
    ("boot",    0, 13),   # 0:00-0:24   silence + sub drone, riser into core
    ("core",   13, 16),   # 0:24-0:54   beat enters, pads, arp builds
    ("tunnel", 29, 16),   # 0:54-1:24   full drive: hats, bass 8ths, fast arp
    ("city",   45, 18),   # 1:24-1:58   groove; breakdown + riser at the end
    ("corrupt",63, 16),   # 1:58-2:28   glitch stutter kicks, detuned bass
    ("dream",  79, 24),   # 2:28-3:13   break: big pads, sparse arp, warm
    ("ocean", 103, 16),   # 3:13-3:43   deep sub pulses, swell, riser
    ("failure",119,16),   # 3:43-4:13   climax: hard driving, chaos
    ("final", 135, 10),   # 4:13-4:32   near-silence, shimmer, final impact
]
TOTAL_BARS = 145
DUR = TOTAL_BARS * BAR    # 271.875 s
N = int(DUR * SR)

rng = np.random.default_rng(0xC0FFEE)

# --- chord progression: roots (MIDI) + quality, one chord per bar ------------
# Cycles A-minor-ish emotional movement; shifts per bar to match the engine's
# per-bar palette hue (chordHue) so the mood arc feels synced.
ROOTS = [45, 41, 48, 43, 45, 41, 48, 40,
         38, 45, 41, 43, 45, 48, 41, 40]
QUAL =  [0,  0,  1,  0,  0,  0,  1,  0,
         0,  0,  0,  0,  0,  1,  0,  0]   # 0 = minor, 1 = major
CHORD_TONES = [
    [0, 3, 7, 12],       # minor triad + octave
    [0, 4, 7, 12],       # major triad + octave
]
SCALES = [0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19, 21, 23, 24]

def midi2f(m): return 440.0 * 2.0 ** ((m - 69) / 12.0)

def t_sec(bar, beat=0.0): return bar * BAR + beat * BEAT

def place(buf, start_s, sig):
    i0 = int(start_s * SR)
    if i0 >= N or i0 < 0: return
    n = min(len(sig), N - i0)
    if n <= 0: return
    buf[i0:i0 + n] += sig[:n]

# --- vectorized filtering (FFT convolution) ------------------------------------
_ker_cache = {}
def _kernel(cutoff, taps=1024, high=False):
    key = (round(cutoff), high)
    if key in _ker_cache: return _ker_cache[key]
    k = np.arange(taps) - taps // 2
    fc = cutoff / SR
    h = np.sinc(2 * fc * k) * 2 * fc
    h *= np.hamming(taps)
    h /= h.sum()
    if high:
        imp = np.zeros(taps); imp[taps // 2] = 1.0
        h = imp - h
    _ker_cache[key] = h
    return h

def _fir(x, h):
    n = len(x)
    taps = len(h)
    size = 1
    while size < n + taps: size <<= 1
    y = np.fft.irfft(np.fft.rfft(x, size) * np.fft.rfft(h, size), size)[:n]
    return y

def lowpass(x, cutoff): return _fir(x, _kernel(cutoff))
def highpass(x, cutoff): return _fir(x, _kernel(cutoff, high=True))
def bandpass(x, lo, hi): return highpass(lowpass(x, hi), lo)

def convolve_fft(x, h):
    n = len(x) + len(h) - 1
    size = 1
    while size < n: size <<= 1
    y = np.fft.irfft(np.fft.rfft(x, size) * np.fft.rfft(h, size), size)
    return y[:len(x)]

# --- envelopes / voices --------------------------------------------------------
def env_adsr(n, a, d, s, r):
    t = np.linspace(0, 1, n)
    ea = np.clip(t * n / max(1, a * SR), 0, 1)
    ed = np.clip(1 - (t * n - a * SR) / max(1, d * SR), 0, 1) if d > 0 else np.ones(n)
    es = np.full(n, s)
    er = np.clip(1 - (t * n - (a + d) * SR) / max(1, r * SR), 0, 1) if r > 0 else np.ones(n)
    return np.minimum(np.minimum(ea, np.maximum(ed, es)), er)

def kick(f0=150.0, f1=42.0, dur=0.55, click=0.03):
    n = int(dur * SR)
    t = np.linspace(0, dur, n)
    f = f1 + (f0 - f1) * np.exp(-t * 22.0)
    ph = 2 * np.pi * np.cumsum(f) / SR
    body = np.sin(ph) * np.exp(-t * 9.0)
    cl = int(click * SR)
    sig = body.copy()
    if cl > 0:
        sig[:cl] += rng.standard_normal(cl) * np.exp(-np.linspace(0, 6, cl)) * 0.5
    return sig * 1.0

def snare(dur=0.22):
    n = int(dur * SR)
    t = np.linspace(0, dur, n)
    noise = rng.standard_normal(n) * np.exp(-t * 22.0)
    tone = np.sin(2 * np.pi * 190 * t) * np.exp(-t * 30.0) * 0.4
    return (noise + tone) * 0.85

def hat_closed(dur=0.07):
    n = int(dur * SR)
    t = np.linspace(0, dur, n)
    return rng.standard_normal(n) * np.exp(-t * 60.0) * 0.35

def hat_open(dur=0.35):
    n = int(dur * SR)
    t = np.linspace(0, dur, n)
    return rng.standard_normal(n) * np.exp(-t * 9.0) * 0.22

def pluck(midi, dur=0.9, bright=0.6):
    n = int(dur * SR)
    t = np.linspace(0, dur, n)
    f = midi2f(midi)
    sig = np.sin(2 * np.pi * f * t)
    sig += 0.4 * np.sin(2 * np.pi * f * 2 * t) * bright
    sig += 0.15 * np.sin(2 * np.pi * f * 3 * t) * bright * bright
    return sig * np.exp(-t * 4.5) * 0.30

def bass_note(midi, dur, detune=0.0, grit=0.0, cutoff=500.0):
    n = int(dur * SR)
    t = np.linspace(0, dur, n)
    f = midi2f(midi)
    sub = np.sin(2 * np.pi * f * t)
    saw = np.zeros(n)
    for h in range(1, 7):
        saw += (1.0 / h) * np.sin(2 * np.pi * f * h * t + rng.uniform(0, 6.28))
    saw = saw / 2.0
    saw2 = np.sin(2 * np.pi * (f * 1.005) * t) * 0.5 if detune else np.zeros(n)
    sig = sub * 0.9 + (saw + saw2) * 0.5 * grit + saw * 0.5 * (1 - grit)
    lp = lowpass(sig, cutoff)
    return lp * env_adsr(n, 0.005, dur * 0.5, 0.35, 0.06)

def pad_chord(midi_roots, quality, dur, cutoff=1400.0, level=0.16):
    n = int(dur * SR)
    t = np.linspace(0, dur, n)
    sig = np.zeros(n)
    for r in midi_roots:
        for dt in CHORD_TONES[quality]:
            f = midi2f(r + dt)
            sig += np.sin(2 * np.pi * f * t + rng.uniform(0, 6.28))
            sig += 0.25 * np.sin(2 * np.pi * f * 2 * t + rng.uniform(0, 6.28))
    sig /= (len(midi_roots) * 2.0)
    sig *= 1.0 + 0.06 * np.sin(2 * np.pi * 0.3 * t + rng.uniform(0, 6.28))
    lp = lowpass(sig, cutoff)
    return lp * env_adsr(n, 0.6, dur * 0.3, 0.8, 1.2) * level

def riser(dur, band=0.0):
    """noise sweep up; band 0 = soft (into core), 1 = loud (climax)"""
    n = int(dur * SR)
    t = np.linspace(0, dur, n)
    noise = rng.standard_normal(n)
    lp = lowpass(noise, 2200.0)
    env = (t / dur) ** 1.5
    return lp * env * (0.10 + 0.30 * band)

def impact(dur=3.5):
    n = int(dur * SR)
    t = np.linspace(0, dur, n)
    boom = np.sin(2 * np.pi * np.cumsum(140.0 * np.exp(-t * 6.0) + 30.0) / SR) * np.exp(-t * 3.2)
    noise = rng.standard_normal(n) * np.exp(-t * 10.0) * 0.8
    sub = np.sin(2 * np.pi * 40.0 * t) * np.exp(-t * 4.0) * 0.9
    return (boom * 1.2 + noise + sub) * 0.9

def shimmer(dur):
    n = int(dur * SR)
    t = np.linspace(0, dur, n)
    f = midi2f(93)
    sig = np.sin(2 * np.pi * f * t) * 0.035
    sig += 0.018 * np.sin(2 * np.pi * f * 1.5 * t)
    trem = 0.5 + 0.5 * np.sin(2 * np.pi * 0.4 * t + 1.3)
    return sig * trem

# --- cheap reverb (FFT convolution with decaying noise IR) ----------------------
def make_ir(sec=1.4, decay=4.5):
    n = int(sec * SR)
    t = np.linspace(0, sec, n)
    return (rng.standard_normal(n) * np.exp(-t * decay)) * 0.5

def reverbed(sig, ir, wet=0.35):
    return sig + convolve_fft(sig, ir) * wet

# --- master buses ---------------------------------------------------------------
master = np.zeros(N)
dry = np.zeros(N)          # kick + bass (no reverb)
wet = np.zeros(N)          # pads, arps, snares, risers (reverb send)

# ==========================================================================
# build events
# ==========================================================================

# ---- BOOT: dark sub drone, slow pulsing, riser into core --------------------
for bar in range(0, 13):
    f = midi2f(ROOTS[bar % 16])
    n = int(BAR * SR)
    t = np.linspace(0, BAR, n)
    place(wet, t_sec(bar), np.sin(2 * np.pi * f * 0.5 * t) * 0.05)
    if bar % 4 == 0:
        place(dry, t_sec(bar, 1.5), bass_note(ROOTS[bar % 16] - 12, BAR * 1.2, grit=0.0))
place(wet, t_sec(12, 3.0), riser(BAR * 0.9, band=0.0))

# ---- CORE: 4-on-floor enters, pad + arp build --------------------------------
for bar in range(13, 29):
    bi = bar - 13
    root = ROOTS[bar % 16]
    qual = QUAL[bar % 16]
    for b in range(4):
        place(dry, t_sec(bar, b), kick())
    place(dry, t_sec(bar, 0.0), bass_note(root, BEAT * 1.8, grit=0.35, cutoff=420.0))
    place(dry, t_sec(bar, 2.0), bass_note(root - 12, BEAT * 1.8, grit=0.35, cutoff=420.0))
    if bi >= 4:
        place(wet, t_sec(bar), pad_chord([root, root - 12], qual, BAR, cutoff=1100.0, level=0.13))
    if bi >= 8:
        for k in range(16):
            tone = SCALES[(k * 2 + bi) % len(SCALES)]
            place(wet, t_sec(bar, k * 0.25), pluck(root + tone - 12, dur=0.6, bright=0.5))
    if bi >= 12:
        for k in range(8):
            place(wet, t_sec(bar, k * 0.5 + 0.25), hat_closed())
place(wet, t_sec(28, 2.0), riser(BAR * 1.2, band=0.3))

# ---- TUNNEL: full drive --------------------------------------------------------
for bar in range(29, 45):
    bi = bar - 29
    root = ROOTS[bar % 16]
    qual = QUAL[bar % 16]
    for b in range(4):
        place(dry, t_sec(bar, b), kick())
    for k in range(8):
        n = root if k % 2 == 0 else root - 12
        place(dry, t_sec(bar, k * 0.5), bass_note(n, 0.42, grit=0.5, cutoff=600.0))
    if bi >= 4:
        place(wet, t_sec(bar, 1.0), snare())
        place(wet, t_sec(bar, 3.0), snare())
    for k in range(16):
        tone = SCALES[(k * 2 + bi * 3) % len(SCALES)]
        place(wet, t_sec(bar, k * 0.25), pluck(root + tone - 24, dur=0.5, bright=0.7))
    for k in range(8):
        place(wet, t_sec(bar, k * 0.5 + 0.25), hat_closed())
    place(wet, t_sec(bar, 3.5), hat_open())
    place(wet, t_sec(bar), pad_chord([root, root - 12], qual, BAR, cutoff=900.0, level=0.09))
place(wet, t_sec(44, 2.5), riser(BAR * 1.3, band=0.4))

# ---- CITY: groove + breakdown ---------------------------------------------------
for bar in range(45, 63):
    bi = bar - 45
    root = ROOTS[bar % 16]
    qual = QUAL[bar % 16]
    for b in range(4):
        place(dry, t_sec(bar, b), kick())
    for k in range(8):
        n = root if (k % 4) != 3 else root + 2
        place(dry, t_sec(bar, k * 0.5), bass_note(n, 0.40, grit=0.45, cutoff=520.0))
    place(wet, t_sec(bar, 1.0), snare())
    place(wet, t_sec(bar, 3.0), snare())
    for k in range(8):
        place(wet, t_sec(bar, k * 0.5 + 0.25), hat_closed())
    place(wet, t_sec(bar, 3.5), hat_open())
    place(wet, t_sec(bar), pad_chord([root, root - 12], qual, BAR, cutoff=1000.0, level=0.10))
    if bi < 14:
        for k in range(16):
            tone = SCALES[(k * 3 + bi) % len(SCALES)]
            place(wet, t_sec(bar, k * 0.25), pluck(root + tone - 24, dur=0.45, bright=0.6))
for bar in range(59, 63):
    root = ROOTS[bar % 16]
    place(wet, t_sec(bar), pad_chord([root, root + 7, root + 12], 0, BAR * 1.1, cutoff=1300.0, level=0.16))
    for k in range(4):
        place(wet, t_sec(bar, k), pluck(root + SCALES[k * 2], dur=0.8, bright=0.5))
place(wet, t_sec(62, 2.5), riser(BAR * 1.3, band=0.5))

# ---- CORRUPT: glitchy stutter kicks + detuned bass ------------------------------
for bar in range(63, 79):
    bi = bar - 63
    root = ROOTS[bar % 16]
    for b in range(4):
        if rng.random() < 0.28:
            for k in range(3):
                place(dry, t_sec(bar, b + k * 0.125), kick(f0=130.0) * 0.7)
        else:
            place(dry, t_sec(bar, b), kick())
    for k in range(8):
        n = root if k % 2 == 0 else root - 12
        det = 0.12 if k % 4 == 3 else 0.0
        place(dry, t_sec(bar, k * 0.5), bass_note(n, 0.42, detune=det, grit=0.8, cutoff=700.0))
    place(wet, t_sec(bar, 1.0), snare())
    place(wet, t_sec(bar, 3.0), snare())
    for k in range(8):
        place(wet, t_sec(bar, k * 0.5 + 0.25), hat_closed())
    place(wet, t_sec(bar, 0.0), pluck(root + 1, dur=0.7, bright=0.9) * 0.5)
    place(wet, t_sec(bar, 2.0), pluck(root + 1, dur=0.7, bright=0.9) * 0.5)
    place(wet, t_sec(bar), pad_chord([root, root - 12], 0, BAR, cutoff=800.0, level=0.11))
place(wet, t_sec(78, 3.0), riser(BAR * 1.1, band=1.0))

# ---- DREAM: break, spacious ------------------------------------------------------
for bar in range(79, 103):
    bi = bar - 79
    root = ROOTS[bar % 16]
    qual = QUAL[bar % 16]
    place(wet, t_sec(bar), pad_chord([root, root + 7, root + 12, root - 12], qual,
                                     BAR * 1.4, cutoff=1600.0, level=0.11))
    step = 0.5 if bi < 16 else 0.25
    kk = 0
    while kk * step < 4.0:
        tone = SCALES[(kk * 3 + bi * 2) % len(SCALES)]
        place(wet, t_sec(bar, kk * step), pluck(root + tone - 12, dur=1.0, bright=0.35) * 0.45)
        kk += 1
    if bi % 2 == 0:
        place(wet, t_sec(bar, 1.0), hat_open() * 0.5)
place(wet, t_sec(102, 2.0), riser(BAR * 1.4, band=0.7))

# ---- OCEAN: sub pulses, swelling -------------------------------------------------
for bar in range(103, 119):
    bi = bar - 103
    root = ROOTS[bar % 16]
    n = int(BAR * SR)
    t = np.linspace(0, BAR, n)
    f = midi2f(root - 24)
    sub = np.sin(2 * np.pi * f * t) * (0.35 + 0.3 * np.sin(2 * np.pi * 0.25 * t))
    env = np.clip(np.linspace(0, 1, n) * 2, 0, 1) * np.clip(2 - np.linspace(0, 1, n) * 2, 0, 1)
    place(dry, t_sec(bar), sub * env)
    if bi >= 8:
        for b in range(4):
            place(dry, t_sec(bar, b), kick() * 0.6)
    place(wet, t_sec(bar), pad_chord([root, root + 7], QUAL[bar % 16], BAR * 1.3,
                                     cutoff=1200.0, level=0.14))
    for k in range(4):
        place(wet, t_sec(bar, k * 0.5), pluck(root + SCALES[k * 2] - 12, dur=0.7, bright=0.4) * 0.5)
place(wet, t_sec(118, 2.5), riser(BAR * 1.4, band=1.0))

# ---- FAILURE: climax --------------------------------------------------------------
for bar in range(119, 135):
    bi = bar - 119
    root = ROOTS[bar % 16]
    if bi >= 6:
        for k in range(8):
            place(dry, t_sec(bar, k * 0.5), kick(f0=140.0, dur=0.4) * (0.85 if k % 2 == 0 else 0.6))
    else:
        for b in range(4):
            place(dry, t_sec(bar, b), kick(f0=140.0, dur=0.4))
    for k in range(16):
        n = root if k % 4 < 3 else root + 1
        place(dry, t_sec(bar, k * 0.25), bass_note(n, 0.22, detune=0.06, grit=0.9, cutoff=800.0))
    place(wet, t_sec(bar, 1.0), snare())
    place(wet, t_sec(bar, 3.0), snare())
    for k in range(8):
        place(wet, t_sec(bar, k * 0.5 + 0.25), hat_closed())
    place(wet, t_sec(bar, 1.5), hat_open())
    place(wet, t_sec(bar, 3.5), hat_open())
    for k in range(8):
        tone = SCALES[(k * 5 + bi * 3) % len(SCALES)]
        place(wet, t_sec(bar, k * 0.5), pluck(root + tone - 12, dur=0.4, bright=1.0) * 0.4)
place(wet, t_sec(134, 3.5), riser(BAR * 1.2, band=1.0))

# ---- FINAL: near-silence + shimmer, then the impact -------------------------------
for bar in range(135, 144):
    bi = bar - 135
    root = ROOTS[bar % 16]
    place(wet, t_sec(bar), shimmer(BAR))
    if bi == 6:
        place(wet, t_sec(bar), pad_chord([root, root + 12], 0, BAR * 1.5, cutoff=1000.0, level=0.07))
place(dry, t_sec(143.75), impact(4.0))

# ==========================================================================
# reverb send + master
# ==========================================================================
ir = make_ir()
wet = reverbed(wet, ir, wet=0.30)
master = dry + wet

# soft clip + normalize to a healthy level
master = np.tanh(master * 1.15)
peak = np.max(np.abs(master))
master = master / peak * 0.92

fade_in = int(2.0 * SR)
master[:fade_in] *= np.linspace(0, 1, fade_in)
fade_out = int(0.3 * SR)
master[-fade_out:] *= np.linspace(1, 0, fade_out)

# stereo: slight width via a tiny delay on the right channel
left = master
right = np.roll(master, int(0.011 * SR))
stereo = np.stack([left, right], axis=1).astype(np.float32)

out = "data/neural_dust/track.wav"
os.makedirs(os.path.dirname(out), exist_ok=True)
pcm = (np.clip(stereo, -1, 1) * 32767).astype(np.int16)
with wave.open(out, "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes(pcm.tobytes())
print(f"wrote {out}: {DUR:.2f}s, {pcm.nbytes/1e6:.1f} MB")
