/*
 * Init.c  -- device library initialisation for pa6t_eth.device.
 *
 * Called by the AmigaOS 4 library manager when the device is first loaded
 * (either automatically via RTF_COLDSTART at boot, or on demand).  We locate
 * the PASemi MAC, DMA and IOB on-chip PCI devices, initialise the hardware
 * DMA rings, install the interrupt handler, and allocate the single unit struct.
 */

#include "pa6t_eth.h"
#include <exec/exec.h>

extern const APTR devInterfaces[];

struct Library *_manager_Init(struct Library *library, BPTR seglist,
                              struct Interface *exec)
{
    struct PA6TEthBase *base = (struct PA6TEthBase *)library;
    struct ExecIFace   *IExec = (struct ExecIFace *)exec;

    IExec->DebugPrintF("[pa6t_eth] Init: " DEVVERSIONSTRING "\n");

    base->IExec       = IExec;
    base->dev_SegList = seglist;

    IExec->InitSemaphore(&base->tx_lock);

    /* Open expansion.library + PCI interface */
    base->ExpansionBase = IExec->OpenLibrary("expansion.library", 54);
    if (!base->ExpansionBase) {
        IExec->DebugPrintF("[pa6t_eth] Init: failed to open expansion.library\n");
        goto fail;
    }
    base->IPCI = (struct PCIIFace *)IExec->GetInterface(
        base->ExpansionBase, "pci", 1, NULL);
    if (!base->IPCI) {
        IExec->DebugPrintF("[pa6t_eth] Init: failed to get IPCI interface\n");
        goto fail;
    }

    /* Locate MAC, DMA and IOB on-chip PCI devices */
    if (!PA6TEth_FindDevices(base))
        goto fail;

    /* Open utility.library */
    base->UtilityBase = IExec->OpenLibrary("utility.library", 50);
    if (!base->UtilityBase) {
        IExec->DebugPrintF("[pa6t_eth] Init: failed to open utility.library\n");
        goto fail;
    }
    base->IUtility = (struct UtilityIFace *)IExec->GetInterface(
        base->UtilityBase, "main", 1, NULL);
    if (!base->IUtility) {
        IExec->DebugPrintF("[pa6t_eth] Init: failed to get IUtility\n");
        goto fail;
    }

    /* Allocate the single unit struct (zeroed) */
    struct PA6TEthUnit *unit = (struct PA6TEthUnit *)IExec->AllocVecTags(
        sizeof(struct PA6TEthUnit),
        AVT_Type,           MEMF_SHARED,
        AVT_ClearWithValue, 0,
        TAG_DONE);
    if (!unit) {
        IExec->DebugPrintF("[pa6t_eth] Init: failed to allocate unit\n");
        goto fail;
    }

    IExec->NewList(&unit->read_list_ipv4);
    IExec->NewList(&unit->read_list_arp);
    IExec->NewList(&unit->read_list_other);
    IExec->NewList(&unit->event_list);
    IExec->NewList(&unit->opener_list);
    IExec->InitSemaphore(&unit->opener_lock);
    base->unit = unit;

    /* Initialise hardware: DMA rings, RX buffers, MAC registers */
    if (!PA6TEth_HWInit(base)) {
        IExec->DebugPrintF("[pa6t_eth] Init: HWInit failed\n");
        goto fail;
    }

    /* Install interrupt handler */
    if (!PA6TEth_InstallIRQ(base)) {
        IExec->DebugPrintF("[pa6t_eth] Init: IRQ install failed\n");
        goto fail;
    }
    DPRINTF(IExec, "[pa6t_eth] Init: IRQ handler installed\n");

    /* Library node setup */
    base->dev_Base.dd_Library.lib_Node.ln_Type  = NT_DEVICE;
    base->dev_Base.dd_Library.lib_Node.ln_Pri   = 0;
    base->dev_Base.dd_Library.lib_Node.ln_Name  = DEVNAME;
    base->dev_Base.dd_Library.lib_Flags         = LIBF_SUMUSED | LIBF_CHANGED;
    base->dev_Base.dd_Library.lib_Version       = DEVVER;
    base->dev_Base.dd_Library.lib_Revision      = DEVREV;
    base->dev_Base.dd_Library.lib_IdString      = DEVVERSIONSTRING;

    IExec->DebugPrintF("[pa6t_eth] Init: ready. MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
        (uint32)base->mac_addr[0], (uint32)base->mac_addr[1],
        (uint32)base->mac_addr[2], (uint32)base->mac_addr[3],
        (uint32)base->mac_addr[4], (uint32)base->mac_addr[5]);

    return (struct Library *)base;

fail:
    if (base->unit)        { IExec->FreeVec(base->unit); base->unit = NULL; }
    if (base->IUtility)    { IExec->DropInterface((struct Interface *)base->IUtility); base->IUtility = NULL; }
    if (base->UtilityBase) { IExec->CloseLibrary(base->UtilityBase); base->UtilityBase = NULL; }
    if (base->iob_dev)     { base->IPCI->FreeDevice(base->iob_dev); base->iob_dev = NULL; }
    if (base->dma_dev)     { base->IPCI->FreeDevice(base->dma_dev); base->dma_dev = NULL; }
    if (base->mac_dev)     { base->IPCI->FreeDevice(base->mac_dev); base->mac_dev = NULL; }
    if (base->IPCI)        { IExec->DropInterface((struct Interface *)base->IPCI); base->IPCI = NULL; }
    if (base->ExpansionBase) { IExec->CloseLibrary(base->ExpansionBase); base->ExpansionBase = NULL; }
    return NULL;
}
