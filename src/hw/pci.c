/*
 * hw/pci.c  -- PCI device discovery for the pa6t_eth driver.
 *
 * We need three on-chip PCI devices:
 *   MAC  (GMAC 1G):    0x1959:0xa005
 *   DMA controller:    0x1959:0xa007
 *   I/O Bridge (IOB):  0x1959:0xa001
 *
 * The X1000 (Nemo) board has one GMAC wired to a Vitesse VSC8221 PHY.
 */

#include "pa6t_eth.h"

/*
 * PA6TEth_FindDevices  -- locate and store the three PCI device handles.
 *
 * Returns TRUE on success, FALSE if any device is missing.
 */
BOOL PA6TEth_FindDevices(struct PA6TEthBase *base)
{
    struct ExecIFace *IExec = base->IExec;
    struct PCIIFace  *IPCI  = base->IPCI;

    DPRINTF(IExec, "[pa6t_eth] FindDevices: scanning PCI bus...\n");

    /* ---- MAC (GMAC 1G) ---- */
    base->mac_dev = IPCI->FindDeviceTags(
        FDT_VendorID, PASEMI_VENDOR_ID,
        FDT_DeviceID, PASEMI_DEV_GMAC,
        TAG_DONE);
    if (!base->mac_dev) {
        IExec->DebugPrintF("[pa6t_eth] FindDevices: MAC 0x1959:0xa005 not found\n");
        goto fail;
    }
    DPRINTF(IExec, "[pa6t_eth] FindDevices: MAC found (ECAM=0x%08lX)\n",
            (ULONG)PA6T_MAC_ECAM);

    /* BusMaster deferred to PostRxBuffers (after stale channels are cleared) */

    /* ---- DMA controller ---- */
    base->dma_dev = IPCI->FindDeviceTags(
        FDT_VendorID, PASEMI_VENDOR_ID,
        FDT_DeviceID, PASEMI_DEV_DMA,
        TAG_DONE);
    if (!base->dma_dev) {
        IExec->DebugPrintF("[pa6t_eth] FindDevices: DMA 0x1959:0xa007 not found\n");
        goto fail;
    }
    DPRINTF(IExec, "[pa6t_eth] FindDevices: DMA found (ECAM=0x%08lX)\n",
            (ULONG)PA6T_DMA_ECAM);

    /* ---- I/O Bridge ---- */
    base->iob_dev = IPCI->FindDeviceTags(
        FDT_VendorID, PASEMI_VENDOR_ID,
        FDT_DeviceID, PASEMI_DEV_IOB,
        TAG_DONE);
    if (!base->iob_dev) {
        IExec->DebugPrintF("[pa6t_eth] FindDevices: IOB 0x1959:0xa001 not found\n");
        goto fail;
    }
    DPRINTF(IExec, "[pa6t_eth] FindDevices: IOB found (ECAM=0x%08lX)\n",
            (ULONG)PA6T_IOB_ECAM);

    /* Map pasdma_status area (per-channel DMA status, big-endian native).
     * No device tree on AmigaOS 4 -- use Linux fallback address directly. */
    base->pasdma_status = (volatile uint64 *)0xfd800000UL;
    DPRINTF(IExec, "[pa6t_eth] FindDevices: pasdma_status @ 0xfd800000\n");

    return TRUE;

fail:
    if (base->dma_dev) { IPCI->FreeDevice(base->dma_dev); base->dma_dev = NULL; }
    if (base->mac_dev) { IPCI->FreeDevice(base->mac_dev); base->mac_dev = NULL; }
    return FALSE;
}
