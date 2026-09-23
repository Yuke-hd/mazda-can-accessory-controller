# License and vehicle-data policy

**Status:** confirmed for MCAN-2 (2026-08-11)

This policy applies to source, documentation, signal definitions, recordings,
fixtures, issue attachments, and pull requests in this public repository. It
protects the receive-only vehicle boundary while making a proposed fixture
reviewable by the same checklist every time.

## License boundaries

The project-authored source code, build files, tests, documentation, and
configuration are released under the Apache License 2.0 in the repository
root. The license grants no permission to use project or vehicle trademarks,
and it does not change the repository's documented safety requirements.

Repository artifacts are classified as follows:

| Artifact | Default status | Required treatment |
| --- | --- | --- |
| Project source and tooling | Apache-2.0 | Keep the root `LICENSE` and preserve contributor copyright notices. |
| Project documentation and tests | Apache-2.0 | Mark any third-party excerpt or asset separately; do not imply that a third-party work is Apache-2.0. |
| Generated signal definitions | Provenance-dependent | Record the input files, exact source commit, generator/version, and modifications. Definitions generated from opendbc retain the applicable MIT attribution and are not automatically Apache-2.0. Definitions derived only from project-authored rules must still include evidence and vehicle-validation status. |
| Raw vehicle captures | Never accepted as a repository artifact | Do not commit, paste or attach raw captures or their contents in an Issue/PR, include them in a release, or link to them externally. A PR may describe that private evidence was used without exposing the capture or its contents. They may remain in an approved private workspace only with the data owner's authorization and suitable access controls. |
| Anonymized replay fixtures | Explicitly reviewed fixture | May be committed only after the checklist below is complete, the submitter attests to sharing authority, and a maintainer records the review. Fixtures must contain no credentials, VIN, precise location, or reconstructable private trip. |
| Issue/PR logs and screenshots | Public by default | Treat as publishable. Use synthetic values and sanitized screenshots; never paste raw captures or secrets. |

The repository does not grant a license to personal or third-party vehicle data
merely because a file is committed. A contributor must have the right to share
each submitted data artifact, and maintainers may reject or remove material
whose provenance or privacy status cannot be established.

## Third-party attribution

The project uses comma.ai/opendbc as a candidate signal lead. Its upstream
project is MIT-licensed. `THIRD_PARTY_NOTICES.md` is the attribution index and
must be updated before any opendbc file, generated output, or copied excerpt is
committed. For every imported or generated item, record:

1. upstream repository and file path;
2. exact commit or release, never a floating branch;
3. access date;
4. original license and copyright notice;
5. local modifications or generator steps; and
6. where the attribution appears in the distributed artifact.

Do not combine an opendbc-derived definition with the project's Apache-2.0
notice in a way that suggests the upstream material was relicensed. Keep
vehicle-specific validation evidence separate from upstream provenance: an
opendbc definition is a lead until verified against the target vehicle.

## Vehicle-data handling

The default input for tests is synthetic data. A real capture may be used for
private analysis, but it must not enter this public repository unless it has
been converted to a reviewed anonymized fixture. The following are prohibited
in commits, Issue attachments, PR comments, screenshots, releases, and public
links:

- VINs, registration numbers, keys, immobilizer identifiers, ECU serials, or
  device serials and MAC addresses;
- Wi-Fi passwords, tokens, credentials, private URLs, or filesystem paths that
  expose a person's identity or network;
- precise GPS coordinates, addresses, landmarks, geofences, or a route that
  can identify a person or regular destination;
- absolute capture dates/times, timezone-linked timestamps, or trip sequences
  that reveal a person's routine;
- unreviewed raw CAN data, diagnostic responses, or files whose unknown bytes
  could identify the vehicle or its owner; and
- personal notes, photos, audio, screen recordings, or metadata containing any
  of the above.

An anonymized fixture must use a synthetic fixture name and a relative timebase
starting at zero (or a deliberately coarsened timebase). Remove source paths,
hostnames, network metadata, and file metadata. Replace or remove fields that
are not needed for the test. Preserve only the frames and scenario labels
needed to reproduce a decoder or protocol behavior. When a field cannot be
shown to be safe, remove it or keep the data private; do not guess.

Anonymization is not a vehicle-control permission. Firmware and vehicle builds
remain receive-only, and no fixture may justify adding a transmit path,
diagnostic polling, or a gateway. Captures must never be collected while
driving merely to inspect logs; follow the staged safety procedure in
the repository's contribution guidance.

## Fixture review checklist

Use this checklist in the PR description or an accompanying review record.
A proposed fixture is publishable only when every applicable item is checked:

- [ ] The fixture has a synthetic filename and contains no VIN, registration,
      ECU/device serial, MAC address, credential, token, or private URL.
- [ ] Absolute timestamps, timezone, GPS/location, route, landmarks, and
      routine-identifying trip patterns are removed or irreversibly coarsened.
- [ ] File metadata, source paths, hostnames, screenshots, and sidecar notes
      have been inspected and sanitized.
- [ ] Raw bytes and unknown CAN IDs were reviewed for identifiers, diagnostic
      data, counters, or other values that could identify the vehicle; unsafe
      fields were removed or the fixture was rejected.
- [ ] The fixture contains only the minimum frames/scenarios needed by its
      named test and uses a relative timebase where timing matters.
- [ ] The source, exact commit, license, and transformation are recorded for
      every third-party or generated definition used to interpret it.
- [ ] The contributor has completed the authorization statement below.
- [ ] A maintainer recorded the review date, reviewer, fixture purpose, and
      any residual uncertainty before merge.

If any item is unknown, the artifact is not an anonymized fixture yet. Keep it
private and request clarification rather than making an ad hoc privacy call.

## Contributor authorization

Every contributor submitting a reviewed anonymized fixture, generated signal
definition, or third-party-derived artifact must include this statement in the
PR (or explicitly state that it is not applicable). Raw captures are never
accepted as repository artifacts.

> I confirm that I own or have permission to submit this artifact, that I have
> followed the license and attribution requirements, and that I have removed
> credentials, VIN/vehicle identifiers, precise location, absolute time, and
> non-anonymized trip data. I understand that this public repository may
> redistribute an accepted fixture under its recorded license and that the
> maintainers may reject or remove it if its provenance, privacy, or safety
> status cannot be verified.

This statement is a contributor assertion, not a waiver of another person's
privacy or a substitute for maintainer review. It does not authorize active CAN
transmission or any use outside the project's documented safety boundary.
