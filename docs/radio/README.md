# Radio

The reason the project exists: a private 868 MHz link to battery sensor nodes,
with no vendor cloud in the path.

Target shape — TDMA slots, frequency hopping, authenticated encryption, up to 64
devices on FSK. Not all of it is built yet; each page states its own status.

| Page | Subject | Status |
|---|---|---|
| [phy.md](phy.md) | modulation, channel plan, duty cycle budget | implemented |
| [driver.md](driver.md) | the RFM69 driver and why it was rewritten | implemented |
| [tdma.md](tdma.md) | superframe and slot structure | geometry and uplink receive done |
| [timebase.md](timebase.md) | why a tick is not a microsecond, and the LSE calibration | implemented |
| [hopping.md](hopping.md) | how the channel for a superframe is chosen | implemented |
| [joining.md](joining.md) | how an unpaired device finds the hub | implemented |
| [pairing.md](pairing.md) | the join region, the quiesce, and the key exchange | hub side done, not yet on air |

Crypto that rides on this link is documented separately under
[security/](../security/) — the split is deliberate, because the wire format has
to be implementable by devices that share none of the hub's code.

## Layering

```
  radio.c (CM4)        frames, state machine, pairing window and quiesce
      |
      +-- hop.c        which channel this superframe uses
      +-- rfm69_lib    register-level chip driver
              |
              +-- rfm69_io_t  SPI / CS / RESET / delay / micros
```

The lowest layer is deliberately platform-free: `rfm69_lib` reaches the chip only
through callbacks, so it runs against a fake register file on a host. See
[driver.md](driver.md).

The genuinely shared layer between hub and device is **not** the driver — the
device has its own radio silicon — it is the protocol: `Common/inc/hop.h`,
`Common/inc/radio_protocol.h`, and the wire crypto contract. See
[ADR-0012](../decisions/0012-wire-format-is-the-contract.md).

## Hardware

RFM69HCW (SX1231) on SPI1, with DIO0 and DIO4 on EXTI lines. Reset is active
high. Timing comes from TIM2 free-running at 1 MHz over its full 32 bits, disciplined
against the LSE crystal because the ST-Link MCO feeding HSE is 0.4% fast — see
[timebase.md](timebase.md).

## Verifying it

Everything the hub transmits is checkable from the host with an RTL-SDR and no
second board: [testing/sdr.md](../testing/sdr.md). The receive path is not —
pairing, ACKs and retries need a real device.
