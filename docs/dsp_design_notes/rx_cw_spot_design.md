# SPOT: auto zero-beat for CW

Status: **built** — `src/cw_spot.c` (the search), `rx_audio.c` (the
audio it searches), `radio.c`'s `radio_spot()` (the tuning), rigctld's
`\spot`, and the control panel's SPOT button.

When you answer a CW station that is 40 Hz off your dial, you transmit 40
Hz off its frequency. Zero-beating by ear means matching its tone to your
sidetone. SPOT does it in one press: it finds the station's tone and moves
the dial so the station sits exactly on the pitch.

## 1. What it does

- **One shot.** It searches the last 1.5 s of audio once, moves the dial,
  and stops. Nothing tracks the station afterwards.
- **What it searches.** The strongest CW tone within the narrow filter's
  width of the pitch (a 300 Hz filter: 150 Hz either side), or 300 Hz
  either side with the filter out. That is roughly what you are hearing.
- **What it moves.** The dial goes to where RIT was listening plus the
  tone's offset. RIT is cleared, because the dial now does its job, so
  the next transmission lands on the station. XIT is kept: it is an
  offset you chose, for example for split.
- **When it won't.** Not in USB, LSB or DIGITAL, not while transmitting,
  not within 1.5 s of a retune, RIT, pitch or mode change (the audio it
  holds would be from before), and not when no tone stands clearly above
  the noise or a stronger one sits just outside the filter (§3).

It works in CW and CWR. CW hears a station `d` Hz above the listening
frequency at the BFO's frequency plus `d`; CWR hears one `d` Hz below it
there. `rx_audio_spot()` gives the station's offset with the right sign.

## 2. The audio and the search

The audio is stage 2's output in CW and CWR, the same tap the CW decoder
uses, before the narrow filter. The audio thread sums it 16 samples at a
time to 6 kHz into a 2.7 s ring; that costs one add per sample. The search
runs on the thread that asked (rigctld's), not the audio thread.

`cw_spot_find()`:

1. Cuts 1.5 s into half-second segments overlapping by half (5 of them),
   Hann-windowed, and measures the power every 2 Hz across the band in
   each with a Goertzel filter, then averages them.
2. Takes the strongest bin. It has to stand at least 10 dB above the
   median bin. Averaging is what makes that threshold work: in a single
   segment the loudest of a few hundred noise bins sits about 10 dB over
   the median by chance; averaged over 5 segments, about 4 dB. Keyed CW at
   0 dB SNR (in 2500 Hz) stands about 25 dB over it.
3. Fits a parabola, in dB, through the peak and the bins either side for
   the frequency. Keying spreads a CW signal into sidebands evenly either
   side of its carrier, so it doesn't pull the peak.

**The BFO's real frequency.** `vfo.c` makes oscillators in steps of
96000/65536 Hz, so a 700 Hz pitch is really 698.73 Hz. A station exactly
on frequency is heard at 698.73 Hz, so the offset is measured from the
BFO's actual frequency, not the nominal pitch. Measured from 700 Hz, the
first end-to-end test came out 1.27 Hz off.

## 3. A strong station just outside the filter

The first version searched only the filter's band, and its test with a
strong station 55 Hz outside a 150 Hz filter failed: SPOT found a peak
inside the band and would have moved about 100 Hz the wrong way. Keying
sidebands reach a long way. A station 30 dB above the noise, keyed at 20
WPM, measured in the search's own terms (dB over the median):

| Distance from its carrier | 0 Hz | 20 Hz | 40 Hz | 55 Hz | 80 Hz | 120 Hz |
|---|---|---|---|---|---|---|
| Level | 47 dB | 21 dB | 18 dB | 13 dB | 6 dB | about 0 dB |

So the search runs 150 Hz beyond the band on each side, and SPOT moves
only if the strongest tone over that whole stretch is inside the band. A
weak station in the filter beside a strong one just outside gets "no clear
signal" rather than a move onto the strong one's sidebands. That case is
left to tuning by hand. Probing stations up to 50 dB over the noise,
anywhere from 150 to 400 Hz outside a 150 Hz band, made no false moves.

## 4. Measured

`make test-cw-spot` (`src/cw_spot_test.c`), on 1.5 s of synthetic audio,
SNR in 2500 Hz:

| Case | Result |
|---|---|
| Steady tone, 20 dB | 0.03 Hz error |
| Keyed at 12, 20, 35 and 50 WPM, 10 dB | 0.04 Hz error at most |
| Keyed 20 WPM, 0 dB / −6 dB | 0.01 / 0.09 Hz error |
| Keyed 20 WPM, 10 dB, every 7.3 Hz from −140 to +140 Hz | 0.10 Hz worst, none missed |
| Two stations, the other 10 dB down | the stronger |
| A strong station 55 to 145 Hz outside a 150 Hz band | nothing found |
| Noise alone, 200 tries | nothing found |
| Cost, 300 Hz band (600 Hz with the guard) | 11 ms on a desktop PC |

`test-rx-audio` case L runs keyed I/Q through `rx_audio_process()` in CW
and CWR, above and below the dial, at a 600 Hz pitch, with a 150 Hz filter
(a station 100 Hz off: refused) and with the filter out (found): every
offset within 0.02 Hz. `test-radio` checks the dial move, RIT cleared, XIT
kept, rounding to 1 Hz, and each refusal.

## 5. Not built

- Tap the waterfall to SPOT the signal nearest your finger.
- A marker on the spectrum showing where SPOT thinks the signal is.
- SPOT over TCI or Kenwood CAT; it is on rigctld only (`\spot`).
- On-air testing.
