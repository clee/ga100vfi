# GPU0 read-only register capture and analysis

`gpu0-capture` reads named GA100 option, status, and controller registers and
performs two bounded 512-row `CMD_READ` passes. It records partial captures and
restores the prior fuse address on completion or error. The optional Linux
BAR0 adapter scopes access to one selected PCI device and restores its PCI
enable and command state when the mapping closes.

`gpu0-analysis` checks map identity, capture stability, source identity, and
published security aliases. The `contract`, `waveform`, and `leaf-api` crates
provide the data types used by these two crates. Register maps are in
[`crates/gpu0-capture/data/`](crates/gpu0-capture/data/README.md).

```sh
cargo test --workspace
```

Tests use synthetic fixtures. Published option values and physical fuse-row
reads remain separate observations in the capture and analysis APIs.
