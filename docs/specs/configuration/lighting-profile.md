# Controller lighting profile

This document specifies `controller_config`'s reusable lighting profile,
application helper, and RPM features. It builds on the generic rule semantics
in [action engine](../action-engine.md) and output behavior in
[local LED actions](../lighting/local-led-actions.md). The persisted schema and
versioned production example are defined in
[controller configuration](controller-config.md).

## Shared application helpers

`controller_config::apply_lighting_profile()` applies a portable
`LightingProfile` while the action engine is detached. It validates the turn
configuration, binds its turn effects, registers the supplied `LedActionSink`,
adds the three strict-Fresh turn-state rules, then applies the RPM level fill
and red-zone features. This typed helper is used by host composition tests;
the firmware applies the equivalent persisted document through
`controller_config::persisted::apply_controller_config()`. Both helpers keep
the sink-before-rules order as part of their contract.

The result reports the failing stage and index, binding or engine status, and
how many effects, rules and features were applied. The helper does not log,
manage provider, engine or renderer lifecycle, or poll the engine. A failed
step leaves prior setup in place, so its caller must handle failure and fail
off the renderer. The firmware composition and its lifecycle calls are
specified in [firmware composition](../../architecture/firmware-composition.md).

The version 1 persisted form of the default profile, with named actions in
place of numeric `ActionId` values, is compiled from YAML to embedded JSON for
firmware and documented in
[controller configuration](controller-config.md#production-lighting-profile).

### RPM level fill

`components/controller_config` owns the feature: which signal drives the fill,
over which input range, and with which freshness requirement. Neither the
action engine nor the LED adapter learns about RPM; the fill only receives a
0.0..1.0 `SetLevel`.

- `controller_config::RpmLevelFillConfig` holds the input range
  (`RpmRange{min_rpm, max_rpm}`, default 0..6500 rpm), the `ActionId` and the
  `FillEffect`. `min_rpm` and below is an empty fill, `max_rpm` and above a
  full fill, and speeds in between fill linearly. Changing the range changes
  the mapping without any renderer change.
- `range_rule()` builds the polled range rule. It uses `FreshOrUnverified`,
  because the Mazda provider reports RPM as `FreshnessUnverified` and has no
  RPM freshness timeout. Missing, stale or unavailable readings still empty
  the fill.
- `apply()` binds the fill, then adds the rule. A failed binding adds no rule;
  a rejected rule, such as an empty or inverted range (`InvalidRange`), leaves
  the binding, so firmware treats any failure as fatal setup and fails off.

`tests/host/rpm_level_fill_tests.cpp` covers the defaults, endpoints, linear
midpoints, clamping, a configured range, unverified and missing readings, and
both failure paths.

### RPM threshold (red zone)

`components/controller_config` also owns the boolean RPM threshold.
The threshold is controller configuration, not renderer state: the LED sink
only receives `Activate` and `Deactivate` for an ordinary `ActionId`, so the
same action can be handled by any output adapter.

- `controller_config::RpmThresholdConfig` holds the threshold
  (`RpmThreshold{rpm}`, default 6000 rpm), the `ActionId`, the `LedEffect`
  (default `Brake`) and its `EffectPriority`.
- `threshold_rule()` builds the sampled state rule
  `vehicle.engine_rpm Greater threshold`. RPM at or below the threshold keeps
  the action inactive; crossing above emits `Activate` once, staying above
  emits nothing more, and crossing back emits `Deactivate`. There is no
  hysteresis. A non-finite threshold is rejected with `InvalidOperand`.
- Freshness: the rule uses `FreshOrUnverified`, like the level fill. The
  Mazda provider always reports RPM as `FreshnessUnverified`, so a strict
  `Fresh` rule would never fire. Missing, stale or unavailable readings and
  failed reads still deactivate the warning.
- `apply()` binds the effect, then adds the rule. A failed binding adds no
  rule; a rejected rule, such as `DuplicateAction` when the action is already
  driven by the level fill, leaves the binding, so firmware fails off.

`tests/host/rpm_threshold_tests.cpp` covers the default, the rule shape, below
and at the threshold, the crossing in both directions without duplicates, a
configured threshold, a lost reading, a non-finite threshold, the independent
LED binding next to the level fill, a shared action and a failed binding.

## Evidence

`tests/host/lighting_profile_tests.cpp` checks the default production values.
`tests/host/lighting_profile_application_tests.cpp` covers profile application,
turn and RPM behavior, and partial-setup diagnostics.
`tests/host/controller_config_application_tests.cpp` covers persisted
application, mirrored turn fail-off behavior, and the factory brake-pedal
action, which shares the `Brake` effect with the red zone at a higher priority
(200 over 150). The feature-specific host
suites are `tests/host/rpm_level_fill_tests.cpp` and
`tests/host/rpm_threshold_tests.cpp`.
