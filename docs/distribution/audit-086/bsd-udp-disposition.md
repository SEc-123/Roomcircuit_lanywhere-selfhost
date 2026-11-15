# BSD UDP findings disposition

Markdown P2-2 is incorrect as stated: actual BSD IPC commands 5/6 already implement Select/Poll and merge relay UDP readiness. HTML B-06/B-07 correctly identify missing UDP connection and descriptor lifecycle details.

Implemented in the actual BSD hooks:

- Track process ID and program ID, reset reused descriptors, and release the table after the last BSD service for that process closes.
- Duplicate and F_DUPFD descriptors retain shared bind/peer/shutdown state. Closing one alias preserves the others. Full tracking tables close a new untrackable UDP descriptor and return ENOBUFS.
- A virtual connected UDP socket supplies logical peer/bind names and routes Read/Write through the existing virtual receive/send semantics.
- Connecting to a real peer or AF_UNSPEC clears all aliases' virtual peer state. Before changing from a physical peer to a virtual peer, disconnect the real kernel peer and reserve/query a valid local port.
- Connected relay receives and readiness match both remote IP and port. PEEK and nonmatching packets retain their normal queue meaning. Both native relay and direct LAN Play queues implement the filter with default arguments for existing callers.
- Virtual shutdown is reflected by receive/send/readiness. After a virtual room ends, the connection reports unreachable/readiness error instead of blocking on an unrelated real socket.
- Virtual UDP batch-message calls return EOPNOTSUPP. The code does not claim support for RecvMMsg/SendMMsg framing. Physical UDP outside a relay room retains the forwarded implementation.

Validation uses `udp_bsd_ipc_test.py`, which extracts the actual hook implementations and uses real kernel UDP sockets in separate game descriptor namespaces. It verifies peer names, Read/Write, peer filtering and PEEK, Poll readiness, duplicate descriptors, physical reconnect/disconnect, shutdown, logical binding, process cleanup, and table capacity. `tcp_stream_ipc_test.py` still passes actual stream sockets and IPC. Evidence is `../udp-bsd-ipc-linux.log`.

The new API arguments only filter local receive queues. They do not change the server/game wire formats. This verification does not establish compatibility with every commercial game's BSD calling pattern.
