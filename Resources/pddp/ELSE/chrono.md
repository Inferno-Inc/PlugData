---
title: chrono

description: Chronometer/timer

categories:
 - object

pdcategory: Assorted

arguments:
- type: list
  description: sets timer length in minutes/seconds
inlets:
  1st:
  - type: bang
    description: rewinds chronometer or timer
  - type: float
    description: rewinds chronometer or non-zero starts or continues, zero stops
  - type: list
    description: sets timer length in minutes / seconds

outlets:
  1st:
  - type: symbol
    description: current time
  2nd:
  - type: bang
    description: when timer function finishes

flags:
- name: -h
  description: also outputs hours for the chronometer
- name: -t
  description: sets to timer function

draft: false
---

[chrono] is a stopwatch chronometer or a timer.