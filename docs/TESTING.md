# AirDrop Throughput Testing

[Back to README](../README.md) · [Client and service design](CLIENT.md) · [Wi-Fi card compatibility](COMPATIBILITY.md)

This guide describes how to produce AirDrop throughput numbers that can be compared across modes, versions and machines. It is a testing method, not a promise of a particular speed.

Speed reports are only useful when the test condition and the measurement point are reproducible. A single number such as “20 MB/s” is not enough: the result depends on the mode, whether a Wi-Fi station is present, the station and AWDL channels, A-MPDU, the coexistence plan, the file size and whether the timer included the receiver's approval.

## Test matrix

Change one variable at a time. A useful first matrix is:

| # | Mode | Wi-Fi station | AirDrop channel | Notes |
|---|---|---|---|---|
| A | Concurrent | Connected, station on channel 40 | 149 | The common “Wi-Fi and AirDrop together” case |
| B | Concurrent | Explicitly disconnected in nekodrop | 149 | No station, but still Concurrent |
| C | On-Demand | No station | 149 | The card is taken only for the transfer |
| D | Concurrent | Connected, station on channel 149 | 149 | Same-channel control, if the router allows it |

For each condition:

- use the same test file or the same total archive size;
- send 2–3 times, not once;
- record every run, including failures that happened before data transfer;
- do not compare a 95 MiB run with a 190 MiB run without saying so.

## Test file and environment

Use a file large enough to make the transfer phase measurable. A single file around 95 MiB is a good baseline; a two-file archive is only comparable when its `total` is the same and the difference in content is stated.

Record before the first run:

- nekodrop version;
- Windows version and build;
- Wi-Fi card model and driver;
- mode: Concurrent, On-Demand or Off;
- station channel and AWDL channel;
- A-MPDU on/off;
- coexistence plan on/off;
- Bluetooth coexistence on/off;
- whether the receiver is a phone, tablet or Mac, without its name;
- approximate distance and whether the receiver stayed unlocked.

Keep the receiver unlocked and its AirDrop sheet open for every run. A locked phone or a closed AirDrop sheet can remove the peer before the transfer starts; that is a `gone` failure, not a throughput result.

## Procedure

1. Select the mode in the nekodrop UI and wait until the radio is ready.
2. Wait about 10 seconds after the radio is ready before the first send. This avoids measuring startup or channel-settling work.
3. Confirm the state in the UI: mode, channel, A-MPDU and coexistence plan.
4. Start one send. Keep the receiver unlocked and accept the request promptly.
5. Record the timestamps from the engine log or the send event stream.
6. Repeat 2–3 times with the same file and condition.
7. To change condition, wait for the radio to settle again; a mode change, Wi-Fi reconnect or channel move restarts the radio and resets counters.
8. Restore Wi-Fi after the test and confirm that it is connected before ending the session.

For the explicit “disconnect Wi-Fi” condition:

- use nekodrop's own Wi-Fi control, not “disable the adapter” in Windows;
- verify that the state shows no station and the AirDrop channel is unchanged;
- do not change the router channel at the same time;
- after the test, reconnect and confirm the station is back.

Do not run a throughput test while the link is still joining, while a channel move is pending, or immediately after a daemon restart. Those windows produce numbers that look like network performance but are actually startup behavior.

## How to measure

Use the transfer phase, not the request phase:

```text
asking    the receiver is being asked; user approval time belongs here
sending   the first data event; start the timer here
sent      the last data event; stop the timer here
```

For each successful run record:

- `total`: the archive size reported by the sender;
- `payload`: the sum of the original file sizes, if available;
- transfer seconds: `sent - sending`;
- rate: `total / seconds`, in MiB/s (1024-based) and Mbit/s (8.388608× MiB/s);
- request-to-send seconds: `sending - asking`, separately;
- whether the run was `sent`, `failed`, or `cancelled`.

Do not use `asking → sent` as the transfer time. It includes the time a person takes to tap Accept and can vary by many seconds. Report it separately if it is interesting.

`total` is the archive size, not necessarily the sum of the file sizes. A small difference is normal; state which number is used for the rate.

## What to capture

Useful engine log events:

- `ready`, `link` and `peer` for discovery and link state;
- `send` events with `state=asking`, `state=sending`, `state=sent` and failures;
- the failure reason for runs that did not transfer data.

Useful service/radio log lines:

- `mode:` and `starting:` / `stopping:`;
- `ready on channel ...`;
- Wi-Fi join/disconnect/reconnect lines;
- `coex plan:` lines;
- `aggregate:` snapshots with sent/acked counts, `largest`, rate code and gate holds;
- `first frame addressed to us after ...`.

`service.log` may be readable only by an administrator. If it is not readable in the normal session, say so in the report instead of omitting the counters silently. Do not paste the whole log; include only the window around each run.

## Report template

```markdown
### Environment
- nekodrop:
- Windows:
- Wi-Fi card:
- Mode:
- Station channel:
- AirDrop channel:
- A-MPDU:
- Coexistence plan:
- Receiver type:

### Condition
- Same file / same total:
- Why this condition:

### Results
| # | asking | sending | sent | transfer s | MiB/s | Mbit/s | result |
|---|---|---|---|---|---|---|---|
| 1 | | | | | | | |

### Counters
- aggregate:
- sent / acked:
- confirmation rate:
- largest:
- rate code:
- gate holds:

### Key log excerpt
```text
...
```

### Notes
- failures before data transfer:
- radio restarts / Wi-Fi reconnects:
- anything that changed between runs:
```

## Common pitfalls

- **`asking` is not transfer time.** It includes the receiver's approval delay.
- **`total` is archive bytes.** Do not mix it with the sum of file sizes without saying which one produced the rate.
- **A `gone` failure before `sending` is not a throughput sample.** The receiver was not reachable when the send started; record it separately.
- **Do not sum counters across radio sessions.** A mode change, channel move or daemon restart starts a new session.
- **`25 out of step` is an initial sync count**, not a permanent failure; report the later `sync frames rewritten` value with it if available.
- **`DELBA 0` is normal** when the PC ends the Block Ack session itself; it only counts DELBAs received from the peer.
- **`10061` is a TCP refusal from the receiver**, usually while the phone is locked or changing screens; note the phone state instead of treating it as a rate result.
- **Wi-Fi 40 and AirDrop 149 are different channels.** If the router is on 40, do not call the result “same-channel”; a 149 same-channel run needs the router or hotspot on 149.
- **One run is an anecdote.** Use 2–3 valid runs and report the spread, not only the best number.

## Privacy

Before posting logs, reports or screenshots:

1. replace the SSID and BSSID with `<SSID>` and `<BSSID>`;
2. remove MAC addresses, IPv4/IPv6 addresses, peer names, peer IDs, device names, user names and local paths;
3. remove full version hashes if they are not needed; keep the release version;
4. crop screenshots and blur the Wi-Fi name, device name and any file names that identify the user;
5. run two checks: first search for the exact known values, then scan for MAC/IP/UUID/path/email patterns.

Prefer short excerpts over full logs. A full `engine.log` or `service.log` can contain peer names, addresses and saved-network metadata even when the speed numbers look harmless.
