# Service cost invariants and steady workload model

The local workerd test `cost-invariants.test.mjs` creates ten actual rooms with one
host/four viewers each and ten directory subscriptions. Test-only context wrappers
count storage calls and application WebSocket dispatches; production has no test
inspection routes. Sixty automatic ping/pong exchanges caused **zero application
handler calls, zero storage calls and zero new HTTP requests**. Ten room-list reads
caused **zero calls to individual room objects**.

`cost-model.test.mjs` executes the production room/directory/capacity alarm and
outbox implementations against counted in-memory storage over eight simulated
hours. It starts from admitted, attached members and assumes timely automatic
pings, steady membership and no signaling/retry/user changes. It checks retained
memberships, renewed capacity/directory leases and no visible revision changes.
Both simultaneous-alarm orderings are tested; room renewals can satisfy and
reschedule due directory/capacity maintenance before its own alarm fires.

| Operation | Room renewal first | Maintenance first |
| --- | ---: | ---: |
| Storage gets | 43,200 | 43,680 |
| Storage puts | 33,600 | 34,080 |
| Storage deletes | 0 | 480 |
| List calls / returned rows | 4,800 / 48,000 | 5,280 / 52,800 |
| Cross-object requests | 9,600 | 9,600 |
| Alarms | 9,600 | 10,560 |
| Alarm writes | 10,560 | 10,560 |
| Puts + deletes + alarm writes | 44,160 | 45,120 |
| Unchanged-list broadcasts | 0 | 0 |

Regression budgets for this model are 35,000 puts, 45,000 gets and 54,000 listed
rows, plus 10,600 alarm writes and 46,000 combined write operations. Redundant
capacity alarm writes are coalesced without postponing an earlier deadline;
this saves 4,320–4,800 writes versus the original model. These are software operation budgets, **not Cloudflare billing units or
free-plan allowances**. No daily headroom, active-duration, SQL-row billing or
production hibernation acceptance is inferred. Admission/signaling/reconnect loads,
actual account-wide usage and billed rows remain required before
claiming the plan's 50% headroom target.

Run `npm test` and `npm run typecheck` in `signaling-worker`. Final measured
invariants/model artifacts are `build/webrtc/stage4-cost-invariants-final.log` and
`cost-coalesced-final.log`. The complete Worker suite, including the model in
both orderings, deployment-capacity tests and admission-alarm safety, passes
201/201 tests (`stage4-worker-final-201.log`). Type checking also passes.

## Published allowance comparison (checked 2026-09-18)

Cloudflare lists daily Free limits of 100,000 Durable Object requests, 13,000 GB-s,
5 million rows read and 100,000 rows written. SQLite KV operations are billed by
rows; deletes and each alarm write count. Duration uses 128 MB per active object;
eligible idle objects and automatic-response handling avoid additional duration.
These are account allowances, not a local test result.
[Official pricing](https://developers.cloudflare.com/durable-objects/platform/pricing/)

Our inference: the steady model's 45,120 combined write operations leaves only
4,880 operations below a 50,000 half-allowance budget, before admission/signaling,
retries, actual row accounting and unrelated account activity. This is not proof
of 50% headroom. Twelve continuously active objects for eight hours would use
44,236.8 GB-s, exceeding the free duration allowance; hibernation eligibility must
be measured in production. The half-allowance duration budget is 6,500 GB-s, or
50,781.25 aggregate active object-seconds at 0.128 GB. Keep that acceptance gate open.

## Deployment budget

`V2_MAX_ROOMS` is an optional string binding containing a decimal integer from
1 through 500; omission keeps 500. The setting can reduce the safety cap, not
raise the protocol/directory bound. Empty, malformed and out-of-range values
return unavailable for new reservations. Existing reservations can still renew
and release, including when an operator lowers the cap below current occupancy.
The serialized capacity object prevents concurrent creates from overbooking.
Per-room viewer limits remain host-controlled from 1 through 63; lowering a
room's viewer limit preserves existing viewers and blocks new admissions.
No deployment or account settings were changed by these tests.
