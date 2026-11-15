# Pinned setup boundary adaptation

`overlay.json` contains the reviewed replacements of three Apache-2.0 upstream
files at esp-now 2.5.3 / 4432294ef6a6cb274b441c0678414455a4055cff.
The root configure hook checks original or already-patched SHA-256 and refuses
unknown source. A fresh component download therefore receives the same change.
No application depends on hand-edited ignored managed files.

Both receive callbacks call `pairing_security_guard` before peer mutation or
queue allocation. The initiator checks the selected MAC and rechecks queued
packets after state changes. Only a handshake is accepted before key send;
only a 32-byte KEY_RESP is accepted after it. Responder accepts KEY only after
two successful Security1 handshake requests. REQUEST/REST retain their one-byte
layout; INFO retains its eight-byte layout. KEY must decrypt to exactly 32 bytes.
All setup sends have finite waits. Initiator is deliberately single-peer only.
The reached initiator cleanup path now receives its queue's struct into a struct,
not a pointer-sized stack slot; queue payload ownership is otherwise unchanged.

Responder `pairing_response_complete(peer, result)` is called after the bounded
unicast KEY_RESP send attempt, including failure. Unlike SEC_OK, it follows the
SDK's MAC-layer send wait. The application can use `espnow_get_key()` (32 bytes),
copy peer and first 16 bytes as LMK, persist, then schedule restart after callback
return. A failed/timed-out send is uncertain, never proof of display receipt.
Application lifecycle must enforce the physical window/three-failure limit,
setup-only REST, persistence and restart; these are T5b responsibilities. Never
mix SDK and native receive callback ownership or treat asynchronous deinit as a
barrier. `authenticate` consumes a caller-generated key: T5b must save its pending
record before calling it. No active pair is established by this adapter.

Firmware cross-build and host structural tests establish software compatibility,
not over-the-air authentication, mapped-LMK encryption, or plaintext rejection.

Review corrections also enable `sec_enable` solely to allocate the SDK's send/
decrypt key contexts; generic data, forwarding, control, OTA, provisioning and
debug receivers stay disabled. Before decrypting an outer secured payload, the
SDK rejects sizes without at least an IV, authentication tag and payload byte.
Responder response allocation follows configured/client ownership filters and
allocation failure returns before session or client mutation.

One authentication attempt is allowed per setup boot, including failed or
cancelled attempts. The SDK's single-slot queue remains allocated until reboot,
so a callback already in flight after unregister cannot use freed queue storage.
A second start is rejected before queue/state changes. T5b/UI must restart into
fresh setup before another Scan/Pair attempt; no retained queue is reused.
All retry loops, receive waits and sends share the same absolute 30-second
budget, and a receive completing at expiry is discarded before parser entry.

`context_regression.py managed_components/espressif__esp-now` extracts and
compiles actual SDK context allocation/key APIs, responder handler/callback,
initiator callback/start/cleanup and retry/receive loop headers with host stubs.
It runs under ASan/UBSan and verifies allocated key contexts/key retrieval,
cross-peer and allocation-failure rejection, malformed callback inputs, retained
queue after teardown, rejected second start and the absolute deadline boundary.
CTest includes this integration fixture when IDF dependencies have been resolved.
The fake clock, allocator, radio send and Security1 parser are test stubs; this
is an SDK boundary regression check, not a cryptographic handshake or radio test.
