# p4kvm alignment (must match jrowny/p4kvm)

## Root cause of solid green/grey with dma_done climbing

`pixsum256=32768` means every sampled byte is `0x80`.
CSI is delivering *frames*, but the video payload is blank fill — not real HDMI pixels.

BabelBus had diverged from the working p4kvm bridge control:

| Item | p4kvm (works) | BabelBus (broken) |
|------|---------------|-------------------|
| TXOPTIONCNTRL | **0** (non-continuous) | forced CONTCLKMODE=1 |
| VI_MUTE on stream | **0xc0** (AUTO_MUTE) | 0 |
| enable sequence | stream → HPD → CSI_START | continuous-clock kicks |
| reapply | color → lanes → STRT | CTXRST + CONTCLK kick |

## Required behavior (copy of p4kvm)

```c
enable:  VI_MUTE = MASK_AUTO_MUTE (0xc0)
disable: VI_MUTE = MASK_AUTO_MUTE | MASK_VI_MUTE
TXOPTIONCNTRL = 0 always (non-continuous)
CSI_START = MASK_STRT after enable / reapply
NO continuous-clock LP11→HS toggle
```

## Not a resolution problem

720x480 is the DVD source. p4kvm uses fixed CSI size from menuconfig;
BabelBus tracks HAct/VAct. Neither is "narrowing on purpose."
