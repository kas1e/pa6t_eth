# pa6t_eth.device

pa6t_eth.device is AmigaOS 4 SANA-II onboard Ethernet driver for the AmigaOne X1000 (PA6T-1682M integrated MAC, Vitesse VSC8221 PHY)

## Hardware

The driver talks to three on-chip PCI devices:
```
GMAC (0x1959:0xa005) - MAC registers -- address, config, packet control
DMA (0x1959:0xa007) - TX/RX descriptor rings, channel control
IOB (0x1959:0xa001) - Interrupt status/acknowledge (accessed via MMIO)
```
## Building

Requires cross-compiler and AmigaOS 4 SDK. Can also build natively on AmigaOS with minor Makefile tweaks

```
make              # release build
make DEBUG=1      # debug build
make clean
```

Output: `build/pa6t_eth.device`

## Reference

in linux_ref/ you can find Linux PASemi driver source code for reference. Includes
the GMAC network driver (pasemi_mac), platform drivers (DMA, PCI, GPIO MDIO, I2C,
etc.), and relevant kernel headers. Since no public TRM exists for the PA6T-1682M,
the Linux driver code serves as the primary hardware reference.

tools/ 
 - dma_interface - small diagnostic tool, showing whole DMA RX interface table
   and status of the all RX and TX channels.
   
See also `DRIVER_OVERVIEW.txt` for a detailed walkthrough of the driver lifecycle.

## License

GPL-2.0 -- see [LICENSE](LICENSE) for details.
