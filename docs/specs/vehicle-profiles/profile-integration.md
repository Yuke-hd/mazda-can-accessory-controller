# Build selection and configuration compatibility

Status: owner-approved Milestone 8 scope (2026-11-02), tracked by #310 and
implemented through issues #394–#401. This connects the explicit-mapping
[DBC importer](dbc-import.md) and the generated [runtime](profile-runtime.md)
to host and firmware builds, factory configuration and saved rules. Until those
issues merge, the current tree still builds native Mazda only.

The integration reuses existing paths. It adds no request file, recognition
pack, selection descriptor, persisted envelope, legacy bridge, firmware or
profile SHA, startup identity comparison or factory-JSON transaction.

## One selection at the composition root

One CMake cache string, `VEHICLE_PROFILE`, selects the profile for host and
ESP-IDF composition:

| Value | Selection |
| --- | --- |
| `native:mazda` (default) | The existing Mazda decoder, service and compiled catalog. |
| Any other nonempty value | One `mapping.json` path for the imported profile. |

Host and firmware resolve a relative path against the repository root, not the
build directory or the ESP-IDF project directory, so one value selects the same
file in both builds. Absolute paths are accepted. An explicit empty value, a
missing file or a failed import fails configure. An explicit selection never
falls back to Mazda. Changing the selection requires configure and a new image.

```sh
cmake -S . -B /tmp/controller-demo \
  -DVEHICLE_PROFILE=tests/fixtures/vehicle-profiles/dbc/mapping-demo-a.json
cmake --build /tmp/controller-demo --parallel

# After activating the documented ESP-IDF 5.5.4 environment:
idf.py -C firmware/weact-can485-v1.1 -B /tmp/controller-demo-fw \
  -DVEHICLE_PROFILE=tests/fixtures/vehicle-profiles/dbc/mapping-demo-a.json build
```

The WeAct target, pins, bitrate and receive-only CAN input remain the board
contract. The mapping's selected bus is the one physical input; it does not
choose another driver, port or bitrate. No Kconfig vehicle matrix, runtime
upload or switching, automatic detection or extra bus is introduced.

## One generation for every consumer

An imported selection runs the existing importer
(`tools/import_vehicle_profile.py`) once at configure time into the build tree.
Its complete artifact set already holds `controller_signal_catalog.json`, the
generated metadata and runtime headers and the capability report. The
importer's source pins, limits, diagnostics and fail-closed publication apply
unchanged. The `mapping.json`, the DBC files it pins and the generator modules
are configure dependencies; generated output is never committed.

Every consumer of an imported image reads that one generation: the runtime
tables, the rule compiler catalog, the replay provider and BLE discovery. None
can substitute the checked-in Mazda catalog. `native:mazda` keeps the
checked-in `tools/controller_signal_catalog.json`, the existing decoder and
service, and no generated runtime. An imported image instantiates only the
generated service, never the native service too.

Generation runs on the host. Firmware configure executes no target binary and
does not build the host test suite. Companion v1 discovery bytes, reserved
fields and catalog keys stay unchanged.

## Factory configuration

`tools/compile_controller_config.py` compiles the factory document through the
existing dependent build step. The selected compiler catalog becomes an
explicit input and dependency of that step instead of the fixed checked-in
path. The result stays the embedded canonical v1 factory JSON.

For a selection, the factory keeps only the built-in features whose signals,
types, choices and capabilities the selected catalog supports. An unsupported
feature is removed completely: its rules, actions and output bindings, with no
dangling binding. This pruning applies only to built-in factory features.
Explicit user documents stay strict: unknown keys, invalid operands,
unsupported capabilities and over-capacity rules fail.

Native Mazda keeps its current turn, RPM and brake features, including the
owner-approved `fresh_or_unverified` brake opt-in and the unset brake freshness.
Imported default features use `Fresh` rules only. They produce output only for
signals whose binding declares `timeout_ms`; a profile that decodes correctly
but declares no timeouts keeps default output black.

A profile with zero bindings (`all_features_disabled`) compiles valid v1 factory
JSON with empty actions, rules and output bindings plus valid board sink
settings. Pixels start and remain black; discovery is empty and replay observes
no inputs. No Mazda policy is selected implicitly. A valid empty configuration
may start the bounded receive-only provider. A factory validation or apply
failure keeps CAN stopped and BLE repair available under the existing failed-off
startup contract.

A populated profile whose factory features are all unsupported compiles the
same empty factory lighting configuration, while retaining its mapped signal
catalog and decoding for BLE discovery, replay and saved user rules. Factory
output remains black; no Mazda policy is selected implicitly.

## Saved rules

Storage is unchanged: canonical v1 JSON in the existing two-slot backend and
`mazda_config` namespace, the
[override storage](../configuration/controller-config.md#boot-time-override-storage)
bounds, and the companion [config transfer](../companion/config-transfer.md)
detached dry run, read-back, CRC and same-session recovery. Existing saved JSON
remains supported without migration.

A saved document depends only on the public signal contract of the selected
provider: signal keys, value types, enum choices, capabilities and permitted
freshness policy. It never depends on CAN identifiers, DBC records or profile
digests. Boot and BLE saves validate it directly against the selected
provider's catalog with the existing parser and semantic validator. A document
whose requirements hold works after a CAN or profile change; one whose
requirements do not hold is rejected on save and ignored at boot, with the
existing invalid-override flag and factory fallback.

Imported selections reject `fresh_or_unverified` in user documents and the
factory. Native Mazda keeps its existing permitted policies. Imported profiles
get no native handlers, and import success never promotes a signal to `Fresh`
without a declared timeout.

Clean apply recovery after an invalid boot override follows separate
[PR #306](https://github.com/Yuke-hd/mazda-can-accessory-controller/pull/306).
Policy and BLE checks may land before it merges, but final boot integration
waits for #306 and rebases onto its merged behavior instead of copying it.

## Acceptance

Synthetic fixtures provide software evidence only:

| Evidence | Required acceptance |
| --- | --- |
| Host selection | Native Mazda, two populated two-source synthetic mappings and the empty mapping configure and build. Relative paths resolve from the repository root in host and firmware builds. Empty, missing and failed selections fail configure without Mazda fallback. |
| Factory | Unsupported features are removed completely; imported defaults use `Fresh` only; no-timeout and empty profiles stay black. |
| Saved rules | Valid bare v1 documents reuse compatible public signals across selections; incompatible documents and imported `fresh_or_unverified` reject; v1 transfer, read-back and CRC stay unchanged; #306 clean fallback holds. |
| Mazda baseline | Existing Mazda tests stay active, and generated decoding compares against the existing Mazda decoder. |
| Firmware software | Fresh ESP-IDF 5.5.4 production and profiling builds for native, populated two-source and empty selections, plus the maximum supported generated fixture, pass architecture, include, storage and IRAM gates with actual service, storage, DRAM, IRAM and flash evidence. |
| Bench / vehicle | Separate owner-authorized follow-up. No such evidence is supplied here. |
