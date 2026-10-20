# Validation

Validated on Windows with GCC 16.2, C23 / C++23 and Codex desktop 26.1013.4894.0.

- Native Release build: passed.
- CTest: 3/3 suites (quota parser, incremental session reader, reset threshold).
- Widget: 51 Chromium checks passed, including 1-second hover, cancelled hover, chat isolation, persistent widget identity, cached snapshots, delayed/rebuilt toolbar and disposal.
- Native WinHTTP/CDP integration: passed, including transient missing composer without API/widget recreation; synthetic usage data stays distinct from real-host acceptance.
- Installed runtime: mounted successfully in real Codex; source and installed widget hashes match.
- Reset boundary: only remaining quota <=1% with an available reset credit and explicit human confirmation. Redemption was not exercised against real credits.
- Native GUI launcher: desktop and Start menu shortcuts automatically attach to an existing debug-enabled Codex; repeated opens retain one collector. The desktop PID comes from the verified Codex package owning the loopback listener, since activation may return a temporary forwarding process.
- Collector lifetime: an isolated process-exit integration check passed; the collector detaches and exits when its nominated desktop process closes. No automatic client restart is enabled.

The new launcher cold-start path has not been exercised against a fully closed live Codex. Original packaged Start/AppsFolder and previously pinned store entries are outside the shortcut integration.

Speed is sampled from output-token changes between timestamped log events. It includes waits and is not a per-token decoder throughput measurement. Context occupancy is an estimate from the latest request and reported capacity.

Desktop DOM integration is not a stable public extension API. Client updates may require adapter changes. Optional watchdog mode and automatic continuation of interrupted tasks are not part of the validated runtime contract.
