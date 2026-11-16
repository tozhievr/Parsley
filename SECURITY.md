# Security reporting

For a suspected vulnerability, use the repository host's private vulnerability-reporting facility when available. Include the affected revision, platform/toolchain, a minimal grammar/input, expected behavior, and reproducible commands. Do not include credentials or private corpora. If no private reporting facility is configured, contact the repository maintainer before publishing exploit details.

The current runtime contracts and resource limits are documented in `docs/STATUS.md`. Fixes should include a regression case and, for memory failures, a sanitizer or allocation-fault test.