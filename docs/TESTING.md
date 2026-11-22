## Summary

Adds `docs/TESTING.md`, a method for producing AirDrop throughput numbers that can be compared across modes, versions and machines.

The recent speed discussions (Concurrent vs On-Demand, Wi-Fi station on 40 vs no station, coexistence plan on/off) show that a single “MiB/s” number is not enough. The test condition, measurement window, file size and counter reset point all change the result. This document makes those points explicit and gives contributors a repeatable report format.

## Changes

- `docs/TESTING.md`: test matrix, measurement rules, log/counter capture, report template, common pitfalls and privacy checks.
- `README.md`: add one link to `docs/TESTING.md` beside the existing design notes.

Suggested README edit:

```diff
-Design notes: [docs/DESIGN.md](docs/DESIGN.md) (driver, DMA, risks) · [docs/CLIENT.md](docs/CLIENT.md) (service, client, modes).
+Design notes: [docs/DESIGN.md](docs/DESIGN.md) (driver, DMA, risks) · [docs/CLIENT.md](docs/CLIENT.md) (service, client, modes) · [docs/TESTING.md](docs/TESTING.md) (throughput test method).
```

## Why

Issue reports often contain useful numbers but not enough context to compare them:

- whether the timer started at `asking` or `sending`;
- whether `total` is archive bytes or the sum of file sizes;
- whether a Wi-Fi station was present during the transfer;
- whether a radio restart reset the aggregate counters;
- whether a failed run transferred any data at all.

A short, mode-independent guide reduces back-and-forth and makes future reports easier to review.

## Scope

Documentation only. No runtime, driver, installer or protocol changes.

The guide deliberately describes the user-visible UI and the existing log files; it does not promise that internal engine or service messages are a stable public API.

## Test plan

- Check that every link resolves relative to `docs/TESTING.md`.
- Check that the Markdown renders on GitHub (tables, fenced blocks, headings).
- Search the document for private values before publishing: no SSID, BSSID, MAC, IP, peer name/ID, user name or local path.
- Confirm the suggested README link is relative and does not change existing anchors.

## Checklist

- [x] Documentation only; no behavior change.
- [x] Follows the existing `docs/` style and links back to the README.
- [x] Covers Concurrent, On-Demand and explicit Wi-Fi disconnect conditions.
- [x] Separates `asking` from transfer time and `total` from payload.
- [x] Explains `gone`, `25 out of step`, `DELBA 0` and `10061` as report notes, not speed results.
- [x] Includes a privacy/redaction step for logs and screenshots.
- [ ] Maintainer review of wording and placement.
