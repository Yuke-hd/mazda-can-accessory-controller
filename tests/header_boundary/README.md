# Public header boundary fixtures

`check_public_headers_test.py` copies the clean fixture here into a temporary
directory, applies forbidden include or target-path mutations, and checks the
diagnostics from the public-header checker. The tests never modify production
files.

For the gate's purpose, commands, and CTest registration, see the
[architecture and boundary validation guide](../../docs/development/architecture-validation.md#public-header-boundary-gate).
