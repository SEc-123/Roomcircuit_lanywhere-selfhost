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

The app trace brackets the selection IPC and probe cleanup. The module trace brackets DNS handle acquisition, cancellation-thread creation/start/wait/destruction, lookup/free, UDP socket/random/send/poll/receive/close and node commit. Each new operation replaces the previous trace. The fixed `node-select-20261116` marker identifies this diagnostic build.

Trace writes are bounded, flushed and closed before advancing. I/O failure disables that attempt's trace without changing the operation result. Records contain only build/version, node ID, stages and result codes. The module also preserves its IPC TLS message and errno around tracing.

Both UI languages use the same black palette. The existing layout and control actions are preserved. The diagnostic uses the current 0.8.8 version, and requires manual installation over an existing 0.8.8.

## References

- [Latest freeze report](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/issues/3#issuecomment-6898929701)
- [Original logs](https://github.com/SEc-123/Roomcircuit_lanywhere-selfhost/issues/3#issuecomment-6886976995)
