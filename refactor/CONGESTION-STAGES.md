# Initial congestion spike — 2026-09-18

Memory optimization relative to legacy is now backlogged at the user's request;
see BACKLOG.md. This investigation focuses on the initial return-image stall
under a sudden bandwidth collapse. It does not change the production media
policy or claim that the transient latency gate is complete.

## Measured stages

The silent packet fixture now records all fifteen input/image responses as:

- Input submission to observed application in the recording input sink.
- Observed application to consumption of the changed image.
- Total response, checked against the existing phase response samples.
- Maximum modeled packet residence in the affected link during that phase.

Input application is polled, so its timing includes polling overhead. The two
integer-ms response stages sum exactly to the recorded total. Link residence
uses the simulator's delivery timestamp and excludes pump scheduling lateness.
It is a phase maximum over packets, **not** the specific marker packet's delay;
do not subtract it from a response to claim an exact receiver-only delay.
These fields remain confined to the headless fixture, with no user input,
audible output, signaling requests or application tracing added.

The baseline's first impaired response was 1,474 ms: 15 ms to apply input and
1,459 ms to see the return image. Maximum link residence was about 565 ms.
The link had nearly 256 queued packets. Existing sender telemetry showed only
a few ms mean packet-send delay, while receiver buffering rose after loss.
This points to the impaired return path and receiver recovery; it is not
evidence that input dispatch or the modular capture wrapper is slow.

## Rejected production experiments

| Configuration | First impaired response | Result |
|---|---:|---|
| Current production policy | 1,474 ms | Baseline |
| Zero-minimum / 10 ms maximum playout, SDK prerender queue bypassed | 1,266 ms | Still a long stall; reverted |
| Keyframe after a greater-than-50% bitrate cut, limited to once per second | 1,222 ms | Still a long stall; reverted |

All three runs recovered and passed unchanged load/isolation/ownership criteria.
These are single exploratory runs, not statistically established improvements.
Earlier unchanged-production first responses ranged from 797 to 1,304 ms, so
neither experiment establishes a reliable improvement worth retaining. The
zero-minimum trial also would require renewed preset-switch and physical A/V
acceptance. No speculative production change is kept.

The network remains at the original 20-to-4 Mbps capacity step and 256-packet
queue. No packet dropping rule, simulator queue shortening or validation
threshold was changed to make the results look better. Keeping all previous
history also avoids relabeling a recovery pass as a low-latency pass.

## Retained work and next boundary

The runner requires the new stage records on fresh runs, validates nonnegative
integer measurements and exact agreement with existing response totals, and
still accepts older artifacts when explicitly evaluated without the new
requirement. Negative tests reject missing phases/samples, boolean/non-number
values, inconsistent sums and changed total responses. The native socket seam
also checks that a configured 100 ms delay appears in modeled link residence.

Use `scripts/test-room-impairment.py BUILD OUTPUT --scenario collapse` to repeat
the complete silent check. The capacity change, offered-load convergence,
healthy-viewer isolation, frame delivery, input revoke, queue bounds and cleanup
requirements are unchanged. Add `--scenario loss5` for the loss regression.

The next congestion work must distinguish lost-frame dependency recovery from
queued stale video, using the return-path measurements; input dispatch and
capture replacement are not supported targets for this symptom. A full router
queue cannot be cleared retroactively by an application setting. This limitation
does not waive the required physical Gaming image/input tests or the existing
transient-congestion acceptance item.

## Final validation

Both native builds succeed. The retained code passes Release collapse and 5%
loss plus Debug collapse, including the new modeled-delay seam check. All
23 Python evidence tests pass. The retained Release collapse first response is
1,394 ms (10 ms input, 1,384 ms return image); Debug is 1,287 ms (5 / 1,282 ms).
Modeled phase-maximum link residence is approximately 573 ms in each. Release's
first 5%-loss response is 120 ms (10 / 110 ms). Recovery and healthy-viewer
isolation pass, but the severe-collapse transient remains unresolved.

[Compact evidence](evidence/congestion-stages-2026-09-18.json) preserves the
baseline, both rejected trials, final Release/Debug scenarios, per-phase queue
samples, stage measurements and source/binary/report hashes. Raw artifacts are
`build/congestion-stages-baseline`, `build/congestion-immediate-trial`,
`build/congestion-keyframe-trial` and `build/congestion-stages-final-{release,debug}`.
