# RFM69 driver

**Status: implemented, unit-tested on host, running on hardware.**

`CM4/rfm69_lib` is a git submodule holding a driver for the SX1231 / RFM69HCW.
It was rewritten rather than patched; [ADR-0005](../decisions/0005-rewrite-rfm69-driver.md)
records why.

## Shape of the API

The driver reaches the chip only through callbacks:

```c
typedef struct {
    int      (*transfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    void     (*select)(void *ctx, int asserted);   /* 1 while the chip is addressed */
    void     (*reset)(void *ctx, int asserted);    /* RFM69 reset is active high */
    void     (*delay_us)(void *ctx, uint32_t us);
    uint32_t (*micros)(void *ctx);                 /* free-running, wrap allowed */
    void      *ctx;
} rfm69_io_t;
```

`CM4/Core/Src/radio.c` supplies the STM32 implementations of these five and
nothing else crosses the boundary.

The point is **not** portability between the hub and the sensor nodes — the nodes
have their own radio silicon and will never run this code
([ADR-0012](../decisions/0012-wire-format-is-the-contract.md)). The point is that
a driver with injected I/O can be exercised on a host against a fake register
file, which is the only way this layer gets tested at all. There is no second
board to test against and an RTL-SDR cannot answer.

## Register shadows

```c
typedef struct {
    rfm69_io_t io;
    uint8_t op_mode;      /* RegOpMode */
    uint8_t dio_map1;     /* RegDioMapping1 */
    uint8_t dio_map2;     /* RegDioMapping2 */
    uint8_t packet_cfg2;  /* RegPacketConfig2 */
} rfm69_dev_t;
```

Four registers carry bits written from more than one place. Those are shadowed and
always read-modify-written.

This is the single biggest defect the old driver had: it wrote whole registers, so
setting a DIO mapping silently cleared the mode bits and setting a mode silently
cleared the DIO mapping. The symptom was intermittent and looked like a hardware
fault. `rfm69_set_mode` preserves `0xE3`; `rfm69_set_dio` composes through the
shadow.

Other corrections carried over from the rewrite:

- `rfm69_read` masks the address with `0x7F`. Unmasked, a read address with the
  top bit set is a **write** — the old code could corrupt the register it meant to
  inspect.
- The SPI stubs are no longer `__weak` no-ops. A missing platform binding is now a
  link error instead of a driver that silently does nothing.

## Settings in physical units

```c
rfm69_set_carrier_hz(dev, 866500000);
rfm69_set_bitrate(dev, 25000);
rfm69_set_deviation_hz(dev, 25000);
rfm69_set_rx_bandwidth_hz(dev, 100000);
```

Callers state what they want in Hz and bits per second; the conversions to
register values live in one place and are exposed
(`rfm69_carrier_to_frf`, `rfm69_bitrate_to_reg`, …) specifically so the tests can
assert on them.

## Tests

```bash
make -C CM4/rfm69_lib/test check
```

Nine groups against a fake SPI register file. Conversions are pinned to the values
the old driver's magic numbers produced — `frf(868 MHz) == 14221312`,
`bitrate(9600) == 0x0D05` — so the rewrite is provably not a behaviour change in
the arithmetic, only in the structure around it.

## Outstanding

- **DIO polling loops have no timeout.** `rfm69_wait_mode_ready` and
  `rfm69_wait_irq2` take a timeout argument and the callers pass one, but some
  paths in `radio.c` still poll unbounded. A chip that stops responding currently
  hangs the superloop until IWDG2 resets the core — which is at least bounded, but
  it is the watchdog covering for the driver.

## See also

- [phy.md](phy.md) — the settings this driver is asked for
- [testing/host-tests.md](../testing/host-tests.md)
