# 12 — using minibitx to create a simple CW transceiver

Status: stub — blocked on TX support (see
[`03_tx_processing_pipeline.md`](03_tx_processing_pipeline.md)).

## Scope

- What a minimal CW transceiver built around minibitx would need beyond
  what exists today: a TX audio/keying path, sidetone generation
  (locally or in the connected app), and how CW keying would be
  signaled to `radio_set_tx()` alongside voice/PTT use.
- Whether keying rides the same rigctld/HPSDR control surfaces documented
  in [`04_remote_control_and_iq_output.md`](04_remote_control_and_iq_output.md)
  or needs its own path (e.g. a hardware key line vs. software keying
  from the connected app).
- This doc is a placeholder until TX exists; revisit its scope once
  `03_tx_processing_pipeline.md` describes real TX behavior.
