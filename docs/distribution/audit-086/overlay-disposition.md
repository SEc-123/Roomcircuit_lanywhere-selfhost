# Overlay findings disposition

Scope is the actual vendored `console/libtesla/include/tesla.hpp` and LANywhere overlay, version unchanged at 0.8.6.

| Finding | Objective conclusion | Resolution and evidence |
| --- | --- | --- |
| HTML O-01 | Overlay logging readback is correct. The failure path is in the sysmodule's log implementation. | Assigned to native remediation. No duplicate UI change. |
| HTML O-02 | Confirmed unchecked thread/event initialization, event created after thread start and closed before thread join. Shared booleans and background GUI hide were cross-thread accesses. | Create combo event before thread, check each result, release only owned handles on failure, atomic running/open/failure/hide request, hide GUI on render thread, join before event close. Tests inject all initialization failure boundaries. |
| HTML O-03 / MD P0-3 focus | Confirmed invalid iterator arithmetic after vector erase and focus underflow when removing the first entry. Empty and upward focus traversal also had boundary errors. | Compute index before erase, clamp focus after removal, adjust insertions, prevent empty traversal and unsigned reverse-loop wrap, own pending additions through destruction. Actual List class tested with debug iterators and UBSan. |
| HTML O-04 / MD P0-3 language | Saved language and system-language font selection differed. Cannot conclude all Chinese text was blank on every system from source alone. | Optional simplified Chinese shared-font fallback independent of system language; both glyph rendering and text width use the same font selection. Corrected overlay README to saved app language. Host fallback tests and target build pass. |
| MD P0-3 cache | Retention was unbounded across new glyph/size keys, but repeated draws of identical text reused cache entries. “Every refresh leaks” is overstated. | Fixed-capacity cache has 512 slots and 256 KiB bitmap budget, frees evicted/exit/reinitialized glyphs, retries transient bitmap allocation failure. Test renders 2000 keys, checks peak bytes, size cap and complete free. |

Host tests are `console/overlay/tests/tesla_dependency_test.py` plus the existing `overlay_test.py`. They exercise actual vendored method bodies with fake font, graphics and kernel boundaries. Tests pass on macOS and Linux Docker. The actual Switch overlay cross-build passed with `TARGET_VERSION=0.8.6`. These results do not provide Switch/Tesla UI or low-resource hardware acceptance.

Vendor file modified: `console/libtesla/include/tesla.hpp`. Preserve the pinned upstream revision and record the local patch and updated source hash in project provenance.
