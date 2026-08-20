# Testing

There is no on-target test framework and no second radio board. What exists is
three separate ways of getting evidence, each covering what the others cannot.

| Page | Covers | Cannot cover |
|---|---|---|
| [host-tests.md](host-tests.md) | logic that runs on a PC — hop sequence, driver register maths | anything involving real hardware |
| [sdr.md](sdr.md) | everything the hub **transmits** | the hub's receive path |
| [on-target.md](on-target.md) | is the firmware actually alive and healthy | correctness |

The gap is deliberate and worth stating: **the hub's receive path — pairing, ACKs,
retries — is not tested by anything.** An RTL-SDR cannot transmit. That needs a
real device, which is a separate effort.

## The habit that matters

Read state off the target rather than guessing from code. Two of this project's
more expensive bugs — the malformed broadcast frame and the Manchester duty cycle —
were invisible in source review and obvious in a capture.
