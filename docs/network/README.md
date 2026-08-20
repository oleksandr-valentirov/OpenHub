# Network

Wired Ethernet on CM7, lwIP in RTOS mode. The radio is the project's reason to
exist; the network side is how a user reaches it.

| Page | Subject | Status |
|---|---|---|
| [ethernet.md](ethernet.md) | lwIP, addressing, the console | implemented |
| [tls.md](tls.md) | secure access to the hub | planned |

See also [architecture/memory-map.md](../architecture/memory-map.md) — the
Ethernet descriptors and lwIP pools dominate RAM_D2 and have a trap in them.
