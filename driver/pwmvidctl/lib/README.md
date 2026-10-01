# Host protocol and planning modules

`pwmvid.py` implements the board protocol and profile validation.
`sequence_plan.py` converts requested time and duty steps into bounded timer
periods. `sequence_run.py` sends a prepared sequence and checks its result.
The CLI uses these modules for both offline planning and live requests.

Keep the CLI and these modules on the same protocol revision. Offline planning
uses the Python standard library.
