# Secure access over Ethernet

**Status: planned.** Nothing is implemented.

## Why

The hub holds presence data and the keys to every sensor
([security/threat-model.md](../security/threat-model.md)). Reaching it over plain
TCP from anywhere on the home LAN means any other host on that LAN — a guest
laptop, a compromised smart appliance — can read and command it.

The console is equally unauthenticated, but it requires physical USB access, which
is a different and much higher bar.

## Approach

**lwIP's `altcp_tls` layer with mbedTLS.** The Cube FW package already ships
`altcp_tls_mbedtls.c`; `altcp` slots underneath the existing TCP API, so
application code changes little.

This is the decisive argument for mbedTLS as the project's one crypto library, and
therefore for using it for the radio's elliptic-curve work as well — see
[ADR-0011](../decisions/0011-mbedtls-on-cm7-only.md) and
[security/crypto-architecture.md](../security/crypto-architecture.md).

## Constraints already known

- **Flash is not a problem.** CM7 uses ~129 KB of a 1 MB bank.
- **RAM needs measuring.** A TLS session costs tens of KB of heap, and CM7's
  `.bss` is already ~340 KB, mostly lwIP pools
  ([architecture/memory-map.md](../architecture/memory-map.md)). The number of
  concurrent sessions has to be bounded deliberately, not left to the heap.
- **AES for TLS is software.** CRYP belongs exclusively to CM4
  ([ADR-0013](../decisions/0013-crypto-peripheral-ownership.md)). At home-LAN data
  rates on a 480 MHz M7 this is not a limitation. HASH is available to CM7 for the
  handshake.
- **Entropy** comes from the RNG under `HSEM_RNG`. mbedTLS's hardware entropy
  hook must call `rng_bytes()` rather than the HAL directly — on this part the HAL
  cannot report a seed error at all. See
  [security/entropy.md](../security/entropy.md).
- **The mbedTLS version shipped by ST is out of support.** See
  [security/crypto-architecture.md](../security/crypto-architecture.md#open-problem-the-shipped-mbedtls-is-out-of-support).

## Open questions

- **Certificate or raw public key?** A self-signed certificate the user pins is
  simplest and needs no CA. X.509 parsing is a large share of mbedTLS's footprint,
  so raw-public-key mode would be smaller — at the cost of client compatibility.
- **What actually runs over it?** A REST/JSON API, MQTT, or a hub-specific
  protocol. Undecided, and it determines how much of lwIP's apps layer is needed.
- **Identity on first use, or provisioned?** The same problem the radio solves with
  an out-of-band fingerprint ([security/key-lifecycle.md](../security/key-lifecycle.md)),
  and it would be consistent to solve it the same way.

## See also

- [ethernet.md](ethernet.md)
- [security/crypto-architecture.md](../security/crypto-architecture.md)
