---
title: cosine~

description:

categories:
 - object

pdcategory: Oscillators

arguments:
  1st:
  - type: float
    description: frequency in hz
    default: 0
  2nd:
  - type: float
    description: initial phase offset
    default: 0

inlets:
  1st:
  - type: float/signal
    description: frequency in hz
  2nd:
  - type: float/signal
    description: phase sync (resets internal phase)
  3rd:
  - type: float/signal
    description: phase offset (modulation input)

outlets:
  1st:
  - type: signal
    description: cosine wave signal

draft: false
---

[cosine~] is a sinusoidal oscillator that accepts negative frequencies, has inlets for phase sync and phase modulation.