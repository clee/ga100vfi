# pwmvidctl

`bin/pwmvidctl` is a command-line client for the PWMVID daemon. It reads status
and profiles, plans timed sequences, and sends explicit arm, background,
sequence, and stop commands. `lib/` contains protocol, profile-validation, and
sequence-planning modules.

The CLI uses a loopback HTTP API and reads its bearer token from a local file
when a command needs the daemon. `plan` operates offline. The supplied
`profiles/` contain measured and extrapolated calibration values for one
board; `examples/` contains commanded waveforms. Measure a new board and
review its wiring and limits before driving it.

```sh
python3 -m unittest discover -s tests -v
./bin/pwmvidctl --help
./bin/pwmvidctl plan examples/gpu0-short-step.json
```

Check daemon and firmware protocol compatibility when combining revisions.
