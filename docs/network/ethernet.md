# Ethernet and addressing

**Status: implemented.**

lwIP runs on CM7 in RTOS mode (`WITH_RTOS 1`), adding `tcpip_thread`,
`ethernetif_input` and `ethernet_link_thread` to the three application threads.

## Addressing

The board boots as a **DHCP client** (`LWIP.LWIP_DHCP=1`). `MX_LWIP_Init` brings
the netif up on `0.0.0.0` and the real address appears once the PHY has
negotiated — **allow about 20 seconds** before concluding it is broken. That delay
has been mistaken for a fault more than once.

Runtime control from the console:

```
ip                                  show current configuration
ip dhcp                             switch to DHCP
ip static <ip> <mask> <gw>          switch to a fixed address
```

The choice is **not persisted** — a reset always returns to DHCP. Persistence waits
on the same configuration store that [key storage](../security/key-lifecycle.md)
needs; `cfg save` / `cfg load` are stubs.

Both paths call into lwIP under `LOCK_TCPIP_CORE()`, because the CLI runs in
`cliTask` and not in `tcpip_thread`. Omitting the lock is not a theoretical
problem: it corrupts netif state under concurrent traffic.

## Console

The CLI is on **USART3 (PD8/PD9)**, which is the ST-Link virtual COM port —
`/dev/ttyACM0`, 115200 8N1 — so it can be driven from a script.

`BSP_COM_Init(COM1, ...)` in `main()` configures the port; `cli_serial_start()`
only adds the RX interrupt, which feeds a queue that `cliTask` blocks on.

**Do not call `getchar` or `scanf`.** `_read` in `newlib_stubs.c` polls the same
USART and would race the ISR, losing characters unpredictably.

UART4 (PC10/PC11) is still configured but unused.

| Command | Purpose |
|---|---|
| `?` | list commands |
| `status` | task table and stack headroom, in **words** |
| `ip` | show or set addressing |
| `ping <addr>` | send an ICMP echo |
| `lwip` | dump lwIP statistics |
| `rng [count]` | draw random words and report RNG health |
| `ipc` | cross-core mailbox header, ring depths, stale replies |
| `crypto` | run the crypto self-tests and interop vectors |
| `rfm <dump\|add\|remove>` | radio subcommands, via the [IPC mailbox](../architecture/ipc.md) |
| `cfg <save\|load>` | **stub** |

## Testing from the host

Serve DHCP on the wired port while keeping the default route where it is —
NetworkManager's shared mode runs a dnsmasq for the subnet and also enables IPv4
forwarding and NAT, which is what gives the board a route out:

```bash
nmcli con mod "Wired connection 1" ipv4.method shared \
      ipv4.addresses 192.168.137.1/24 ipv4.never-default yes
nmcli con up "Wired connection 1"
ip neigh show dev enp6s0        # the board's lease shows up here
ping -c 200 -i 0.002 <lease>
```

Then read `status` and `lwip` on the console. **Healthy** means: no `drop`,
`chkerr` or `err`; `icmp.recv` matching `icmp.xmit`; every task with more than
~100 words of stack free.

## The stack-size trap

Worth repeating here because it presents as a network fault and is not one.

CMSIS-RTOS v2 takes `stack_size` in **bytes**. The lwIP glue passes word counts
straight through — `INTERFACE_THREAD_STACK_SIZE` in `ethernetif.c` and `lwip.c`,
`TCPIP_THREAD_STACKSIZE` and `DEFAULT_THREAD_STACKSIZE` in `sys_arch.c`. Taken as
bytes these are ~4x too small.

The first received frame then overflows the `EthIf` stack and hard-faults the core.
The symptom is a **dead CLI with a frozen `xTickCount`** — which looks like a
console bug, a scheduler bug, anything but lwIP. Re-check the `* 4` after every
CubeMX regeneration.

## See also

- [tls.md](tls.md)
- [architecture/memory-map.md](../architecture/memory-map.md)
- [architecture/build-and-generation.md](../architecture/build-and-generation.md)
