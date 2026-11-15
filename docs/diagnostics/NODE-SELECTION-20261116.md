# OLED node-selection diagnostic build

The default app palette is OLED black. The public-node freeze reported in Issue #3 remains under investigation.

## Evidence

The newly supplied logs.zip is identical to the earlier attachment (SHA-256 `6c95aceb3dbf53eeed06f2abc990b194bc59cde5bf38cdd186643c81bf674648`). Its NRO identifies as 0.8.1. The updater thread enters Sodium's Unix random backend, aborts, and closes HID while the UI is still polling. The HOME Menu report follows one second later.

The actual 0.8.8 release binary still registers the Switch random backend before Sodium initialization. The attached logs contain no trace of the newly reported node-selection failure.

The current A-selection path calls `SetHostNode`, resolves the endpoint, performs a UDP probe for LAN Play nodes, then commits the selected node. The two original endpoints bypass hostname resolution. LAN Play configuration, virtual-address persistence and lwIP initialization happen later. The supplied evidence does not establish which step now freezes on the reporter's console.

The previous DNS allocation-hook fix is present in the released binary. Pinned SDK inspection verified the DNS thread's supplied stack, allowed priority and TLS initialization. Existing host regressions pass, but their simulated kernel/service boundaries cannot establish this device outcome. Earlier claims that the selection freeze was fixed were stronger than the available device evidence.

## Reproduce on the affected console

1. Copy `atmosphere/` and `switch/` from the complete diagnostic SD ZIP to the SD root.
2. Restart Atmosphère emuMMC, open RoomCircuit and select one node that previously froze.
3. After reproducing, collect these files before selecting another node:
   - `lanywhere/node-selection-app.log`
   - `lanywhere/node-selection.log`
4. Include the displayed app version and whether HOME still responds. If a file is absent, report which one.

The app trace brackets the selection IPC and probe cleanup. The module trace brackets DNS handle acquisition, cancellation-thread creation/start/wait/destruction, lookup/free, UDP socket/random/send/poll/receive/close and node commit. Each new operation replaces the previous trace. The fixed `node-select-20261116b` marker identifies this diagnostic build.

Trace writes are bounded, flushed and closed before advancing. I/O failure disables that attempt's trace without changing the operation result. Records contain only build/version, node ID, stages and result codes. The module also preserves its IPC TLS message and errno around tracing.

Both UI languages use the same black palette. The existing layout and control actions are preserved. The diagnostic uses the current 0.8.8 version, and requires manual installation over an existing 0.8.8.

## References

- [Latest freeze report](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/issues/3#issuecomment-6898929701)
- [Original logs](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/issues/3#issuecomment-6886976995)


## Deeper investigation

The node chooser sends a short protocol probe before saving the selected node. LAN Play Node returns the first four request bytes, while Rust returns the type byte and four nonce bytes. Both formats are supported. Room creation, admission and game traffic occur later.

Two independent defects were reproduced and corrected:

- The native UDP probe rejected the node on its first unrelated or malformed datagram, or an interrupted poll. It now keeps the original deadline, ignores unrelated replies and retries interrupted operations. Twenty-one actual-socket cases pass with AddressSanitizer and UndefinedBehaviorSanitizer, including both echo formats, wrong source/nonce, empty/oversized packets, delayed/silent/flooded peers and resource failure.
- The pinned thread-creation wrapper aborted on kernel thread-limit, memory and handle exhaustion before callers could handle failure. Those resource errors now return the existing OS resource result and preserve the outer cleanup. Invalid arguments and invalid state retain their existing error policy. Fault injection through the actual methods and result macros reproduced the old abort and verified the new return and cleanup.

These are confirmed code defects. No new console trace has established which condition caused the reporter's freeze. The UI still performs the selection IPC synchronously, and DNS cancellation is a request to the OS service. A stalled service can therefore stop UI progress without generating a fatal report. A HOME response and the final diagnostic stage help distinguish this from a system-module abort.

On 2026-11-16, three read-only PING attempts from the China relay host succeeded for all thirteen catalog entries. The Mac route used utun4 and synthesized 198.18.0.x DNS answers. Even with public A records, seven UDP entries and the original community TCP probe did not respond normally from that route. These observations describe reachability from those two networks. They do not establish the reporter's Switch route or game admission. Servegame remains excluded from hosting because its earlier admission test failed.

Protocol references: [Node](https://github.com/spacemeowx2/switch-lan-play/blob/1be20e1905f6ed2b2136f29f0c5aa66f2f8de04e/server/src/udpserver.ts), [Rust](https://github.com/spacemeowx2/slp-server-rust/blob/de1fa9fc58a0d3c345163f0b5645b20a6784983e/src/slp/frame.rs).
