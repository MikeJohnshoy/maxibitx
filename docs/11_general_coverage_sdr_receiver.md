# 11 — using minibitx with an SDR app as a general coverage receiver

Status: stub.

## Scope

- Using Quisk, SDR Console, or similar as a general-coverage receiver
  front end for minibitx: HPSDR connection setup, tuning via rigctld.
- Practical receive range and any band-related caveats — the LPF bank
  covers under-30 MHz in four bands (see
  [`01_hardware_init_and_control.md`](01_hardware_init_and_control.md));
  anything about behavior outside that range belongs here.
- Filtering/mode selection responsibilities: entirely on the SDR app
  side, since minibitx passes unfiltered-by-mode baseband I/Q (see
  [`02_rx_processing_pipeline.md`](02_rx_processing_pipeline.md)).
