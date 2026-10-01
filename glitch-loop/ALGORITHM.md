# Source-bound glitch experiment loop

The controller must maintain an immutable record for every attempted physical
issue. The target supplies current boot, selected device, board, image and
calibration identities. A candidate phenotype is held for independent review;
the loop never infers a fuse write route from alias values alone.

```text
enroll target with two matching, independent read-only baselines
verify selected device, current boot, board identity and measured calibration

while target is READY and the search budget permits:
    propose one or two bounded waveform candidates
    compile each candidate without driving the board
    reject invalid, duplicate or out-of-policy candidates

    reserve fresh one-use attempt IDs and persist the exact plan
    verify the reservation and source identities again
    issue each accepted plan at most once

    if an issue response is missing or ambiguous:
        reconcile only through read-only status and durable receipts
        never replay the uncertain physical issue
        hold if issue state cannot be proven

    capture the post-attempt board and GPU state independently
    classify from validated evidence, preserving partial observations
    if the selected published-alias phenotype appears:
        stop and hold the candidate for independent eligibility review
    otherwise use the classified result to choose the next proposal
```

The loop stops at a held candidate for independent eligibility review. A reset,
new specimen, changed firmware image or calibration begins a new source-bound
decision context. Previously issued proposals remain spent.
