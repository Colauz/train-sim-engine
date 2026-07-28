#!/usr/bin/env python3
"""Génère assets/audio/ats_alarm.wav — alarme ATS de cabine (M32).

« Ding... Ding... » de carillon en boucle : deux frappes par période de 1,2 s,
tonal + harmonique avec décroissance de cloche. Boucle sans couture (les frappes
sont revenues à ~0 avant la fin du fichier).
Sortie : 48 kHz, 16 bits, mono (stdlib uniquement).
"""

import math
import os
import struct
import wave

SAMPLE_RATE = 48000
LOOP = 1.2            # durée de la boucle (s)
STRIKES = (0.0, 0.6)  # instants des deux « ding »
FREQ = 1046.5         # do6
HARM = 2.0 * FREQ


def synth():
    n = int(SAMPLE_RATE * LOOP)
    samples = []
    for i in range(n):
        t = i / SAMPLE_RATE
        v = 0.0
        for strike in STRIKES:
            ts = t - strike
            if ts < 0.0:
                continue
            env = math.exp(-ts * 7.0)
            v += (math.sin(2.0 * math.pi * FREQ * ts) * 0.6
                  + math.sin(2.0 * math.pi * HARM * ts) * 0.22) * env
        samples.append(v * 0.55)
    return samples


def main():
    out = os.path.join(os.path.dirname(__file__), "..", "assets", "audio",
                       "ats_alarm.wav")
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
