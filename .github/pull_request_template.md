## Summary

<!-- What changed and why? Keep this focused on observable behavior and design intent. -->

## Related issue

<!-- Use `Closes #123` only when this PR fully satisfies the issue. -->

Refs #

## Scope

### Included

-

### Not included

-

## Safety impact

- [ ] Vehicle firmware remains CAN listen-only and has no business-level transmit API.
- [ ] Startup, stale-data, reset, and failure behavior remain fail-silent.
- [ ] No active CAN test target or `BENCH_ACK_ONLY` artifact can be mistaken for a vehicle build.
- [ ] Not applicable; this PR cannot affect CAN, vehicle builds, or hardware. Explanation:

## Validation

### Automated

- [ ] Relevant host/unit/replay tests passed.
- [ ] Not run. Reason:

### Bench hardware

- [ ] Performed on the isolated CAN bench. Hardware/build/results:
- [ ] Not run. Reason:

### Vehicle

- [ ] Performed on the target vehicle under the documented safety procedure. Results:
- [ ] Not run. Reason:

## Hardware and data evidence

<!-- Board revision, pins, bitrate, capture fixture, DBC source/commit, signal confidence. -->

## Privacy and third-party material

- [ ] No credentials, VIN, precise location, or non-anonymized private trip data are included.
- [ ] No raw vehicle capture is included; raw captures are never accepted.
- [ ] Third-party code/data has source, exact version, and license recorded.
- [ ] If a reviewed anonymized fixture, generated signal definition, or third-party-derived artifact is included, I have completed the policy checklist and authorization statement.
- [ ] Not applicable.

## Risks, rollback, and follow-up

<!-- Known limitations, how to revert safely, and any follow-up issues. -->
