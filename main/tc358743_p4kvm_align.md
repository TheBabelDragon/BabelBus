# p4kvm alignment (must match jrowny/p4kvm + Linux)

## Root cause of TxAct=0 / dma_done_irqs=0 while HDMI locked

SYS_STATUS shows TMDS+SCDT+SYNC, but CSI_STATUS has TxAct=0 RxAct=0 and the
P4 CSI receiver never completes a frame.

BabelBus had diverged from the working path:

| Item | p4kvm / Linux (works) | BabelBus (broken) |
|------|----------------------|-------------------|
| TXOPTIONCNTRL | **0** (non-continuous) | forced CONTCLKMODE=1 |
| VI_MUTE on stream | **0xc0** (AUTO_MUTE) | 0x00 |
| enable sequence | stream → HPD → CSI_START | continuous-clock kicks |
| reapply | color → lanes → STRT | CTXRST + CONTCLK kick |

## Required behavior

```c
enable:  VI_MUTE = MASK_AUTO_MUTE (0xc0)
disable: VI_MUTE = MASK_AUTO_MUTE | MASK_VI_MUTE
TXOPTIONCNTRL = 0 always (non-continuous)
CSI_START = MASK_STRT after enable / reapply / soft_kick
NO permanent continuous-clock mode
```

Linux briefly toggles CONTCLK during enable for LP11→HS on some platforms;
on ESP32-P4 + these adapters the stable path is pure non-continuous as in p4kvm.

## DVI sources (HDMI bit = 0 in SYS_STATUS)

Set VI_MODE RGB_DVI when not HDMI. DVD players often present as DVI 720x480.
