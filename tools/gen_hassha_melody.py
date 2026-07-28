#!/usr/bin/env python3
"""Génère assets/audio/hassha_melody.wav — jingle de départ style métro japonais (M32).

Suite de notes douces en gamme pentatonique (type « départ Yamanote ») : sinus +
2e harmonique, attaque courte et décroissance exponentielle, ~3 s.
Sortie : 48 kHz, 16 bits, mono (stdlib uniquement).
"""

import math
import os
import struct
import wave

SAMPLE_RATE = 48000

# Pentatonique de ré majeur (D5 F#5 A5 B5 D6 ...) — montante puis retombée,
# phrasé « ding-ding-ding-ding » de sonnerie de quai.
NOTES = [
    (587.33, 0.30),   # ré5
    (739.99, 0.30),   # fa#5
    (880.00, 0.30),   # la5
    (1174.66, 0.45),  # ré6
    (987.77, 0.30),   # si5
    (880.00, 0.30),   # la5
    (1174.66, 0.60),  # ré6 (tenue finale)
]
FADE_OUT = 0.25  # secondes de fondu final


def synth():
    total = sum(d for _, d in NOTES) + FADE_OUT
    n = int(SAMPLE_RATE * total)
    samples = [0.0] * n
    t0 = 0.0
    for freq, dur in NOTES:
        start = int(SAMPLE_RATE * t0)
        count = int(SAMPLE_RATE * dur)
        for i in range(count):
            t = i / SAMPLE_RATE
            env = min(1.0, t * 60.0) * math.exp(-t * 3.0)
            v = (math.sin(2.0 * math.pi * freq * t) * 0.7
                 + math.sin(2.0 * math.pi * 2.0 * freq * t) * 0.18) * env
            idx = start + i
            if idx < n:
                samples[idx] += v * 0.6
        t0 += dur
    # Fondu final pour éviter tout clic en fin de fichier.
    fade = int(SAMPLE_RATE * FADE_OUT)
    for i in range(fade):
        idx = n - fade + i
        samples[idx] *= 1.0 - i / fade
    return samples


def main():
    out = os.path.join(os.path.dirname(__file__), "..", "assets", "audio",
                       "hassha_melody.wav")
    samples = synth()
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(b"".join(
            struct.pack("<h", max(-32768, min(32767, int(s * 32767)))) for s in samples))
    print(f"{out} : {len(samples) / SAMPLE_RATE:.2f} s, {SAMPLE_RATE} Hz mono 16 bits")


if __name__ == "__main__":
    main()
