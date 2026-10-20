# Validation

Validated on Windows with GCC 16.2, C23 / C++23 and Codex desktop 26.1013.4894.0.

- Native Release build: passed.
- CTest: 3/3 suites (quota parser, incremental session reader, reset threshold).
- Widget: 51 Chromium checks passed, including 1-second hover, cancelled hover, chat isolation, persistent widget identity, cached snapshots, delayed/rebuilt toolbar and disposal.
- Native WinHTTP/CDP integration: passed, including transient missing composer without API/widget recreation; synthetic usage data stays distinct from real-host acceptance.
- Installed runtime: mounted successfully in real Codex; source and installed widget hashes match.
- Reset boundary: only remaining quota <=1% with an available reset credit and explicit human confirmation. Redemption was not exercised against real credits.

Speed is sampled from output-token changes between timestamped log events. It includes waits and is not a per-token decoder throughput measurement. Context occupancy is an estimate from the latest request and reported capacity.

Desktop DOM integration is not a stable public extension API. Client updates may require adapter changes. Optional watchdog mode and automatic continuation of interrupted tasks are not part of the validated runtime contract.
