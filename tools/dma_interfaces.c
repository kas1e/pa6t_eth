/*
 * dma_interfaces.c -- Query the PA6T-1682M ENVOI DMA engine on the X1000.
 *
 * Output goes to serial (DebugPrintF).
 *
 * The PA6T's DMA engine is part of the ENVOI I/O subsystem which drives
 * the SerDes lanes.  SerDes lanes support PCIe, XAUI, and SGMII protocols.
 * The DMA engine provides packet DMA for lanes configured as network MACs
 * (SGMII/GMAC or XAUI).  Lanes configured as PCIe do their own bus-mastering.
 *
 * On the X1000 (Nemo) board, the 24 SerDes lanes are assigned as follows
 * (from TRM Table 1):
 *
 *   Quad 0-1: PCIe slot 1 (x16) / slot 2 (x8 mux)
 *   Quad 2-3: PCIe slot 1 (x16)
 *   Quad 4:   SB600 southbridge (x4)
 *   Quad 5:   Lane 0 = PCIe slot 3
 *             Lane 1 = PCIe slot 4
 *             Lane 2 = PCIe slot 5
 *             Lane 3 = SGMII -> Ethernet PHY (Vitesse VSC8221)
 *
 * Only Quad 5 Lane 3 is configured as SGMII; all others are PCIe.
 * XAUI is not used (no lanes assigned).
 *
 * The DMA engine has 6 RX interfaces in its interface table, mapping to
 * the 6 SerDes lanes that the silicon *could* use as network MACs:
 *
 *   Interface 0: devfn=0xA8 (dev=21 fn=0) - XAUI (not wired on X1000)
 *   Interface 1: devfn=0xA9 (dev=21 fn=1) - XAUI (not wired on X1000)
 *   Interface 2: devfn=0xA0 (dev=20 fn=0) - Quad 5 Lane 0, PCIe slot 3 (not SGMII)
 *   Interface 3: devfn=0xA1 (dev=20 fn=1) - Quad 5 Lane 1, PCIe slot 4 (not SGMII)
 *   Interface 4: devfn=0xA2 (dev=20 fn=2) - Quad 5 Lane 2, PCIe slot 5 (not SGMII)
 *   Interface 5: devfn=0xA3 (dev=20 fn=3) - Quad 5 Lane 3, SGMII -> Ethernet PHY
 *
 * Shows:
 *   1. DMA RX interface table (with X1000 SerDes assignment)
 *   2. TX channel status and ring base addresses
 *   3. RX channel status and ring base addresses
 *
 * Build: ppc-amigaos-gcc -O2 -Wall -o dma_interfaces dma_interfaces.c -lauto
 * Run:   dma_interfaces
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <expansion/pci.h>
#include <interfaces/expansion.h>
#include <utility/tagitem.h>

#define PASEMI_VENDOR_ID    0x1959

/* PASemi on-chip PCI device IDs (from X1000 Nemo DTS) */
#define PASEMI_DEV_GMAC     0xa005  /* 1G Ethernet GMAC (SGMII) */
#define PASEMI_DEV_XAUI     0xa006  /* 10G Ethernet XAUI */
#define PASEMI_DEV_DMA      0xa007  /* DMA controller */

/* DMA capability registers */
#define PAS_DMA_CAP_TXCH      0x44
#define PAS_DMA_CAP_TXCH_TCHN_M  0x00ff0000
#define PAS_DMA_CAP_TXCH_TCHN_S  16

#define PAS_DMA_CAP_RXCH      0x48
#define PAS_DMA_CAP_RXCH_RCHN_M  0x00ff0000
#define PAS_DMA_CAP_RXCH_RCHN_S  16

#define PAS_DMA_CAP_IFI       0x4c
#define PAS_DMA_CAP_IFI_NIN_M  0x00ff0000
#define PAS_DMA_CAP_IFI_NIN_S  16
#define PAS_DMA_CAP_IFI_IOFF_M 0xff000000
#define PAS_DMA_CAP_IFI_IOFF_S 24

/* TX channel registers (stride 0x20) */
#define PAS_DMA_TXCHAN_TCMDSTA(c)  (0x300 + (c)*0x20)
#define PAS_DMA_TXCHAN_BASEL(c)    (0x318 + (c)*0x20)
#define PAS_DMA_TXCHAN_BASEU(c)    (0x31c + (c)*0x20)
#define PAS_DMA_TXCHAN_TCMDSTA_ACT 0x00010000
#define PAS_DMA_TXCHAN_TCMDSTA_EN  0x00000001

/* RX channel registers (stride 0x20) */
#define PAS_DMA_RXCHAN_CCMDSTA(c)  (0x800 + (c)*0x20)
#define PAS_DMA_RXCHAN_BASEL(c)    (0x818 + (c)*0x20)
#define PAS_DMA_RXCHAN_BASEU(c)    (0x81c + (c)*0x20)
#define PAS_DMA_RXCHAN_CCMDSTA_ACT 0x00010000
#define PAS_DMA_RXCHAN_CCMDSTA_EN  0x00000001

/*
 * X1000 SerDes assignment for DMA-capable lanes.
 *
 * The DMA interface table uses devfn (dev<<3|fn) to identify each lane.
 * We hardcode the X1000 board assignments here since they are fixed in
 * hardware and cannot be discovered at runtime.
 */
static const struct {
    uint8  devfn;
    const char *hw_name;     /* what the silicon calls it */
    const char *x1000_use;   /* what the X1000 board actually does with it */
} x1000_serdes_map[] = {
    { 0xA8, "XAUI (not wired on X1000)",         "unused" },
    { 0xA9, "XAUI (not wired on X1000)",         "unused" },
    { 0xA0, "Quad 5 Lane 0 -- PCIe slot 3",       "PCIe (not SGMII)" },
    { 0xA1, "Quad 5 Lane 1 -- PCIe slot 4",       "PCIe (not SGMII)" },
    { 0xA2, "Quad 5 Lane 2 -- PCIe slot 5",       "PCIe (not SGMII)" },
    { 0xA3, "Quad 5 Lane 3 -- SGMII",             "Ethernet (Vitesse VSC8221 PHY)" },
    { 0, NULL, NULL }
};

/* Look up the X1000 SerDes assignment for a given devfn */
static const char *serdes_hw_name(uint8 devfn)
{
    int i;
    for (i = 0; x1000_serdes_map[i].hw_name; i++)
        if (x1000_serdes_map[i].devfn == devfn)
            return x1000_serdes_map[i].hw_name;
    return "unknown";
}

static const char *serdes_x1000_use(uint8 devfn)
{
    int i;
    for (i = 0; x1000_serdes_map[i].x1000_use; i++)
        if (x1000_serdes_map[i].devfn == devfn)
            return x1000_serdes_map[i].x1000_use;
    return "unknown";
}

int main(void)
{
    struct Library *ExpansionBase;
    struct PCIIFace *IPCI;
    int32 i, j;

    ExpansionBase = IExec->OpenLibrary("expansion.library", 54);
    if (!ExpansionBase) {
        IExec->DebugPrintF("Failed to open expansion.library\n");
        return 20;
    }

    IPCI = (struct PCIIFace *)IExec->GetInterface(ExpansionBase, "pci", 1, NULL);
    if (!IPCI) {
        IExec->DebugPrintF("Failed to get PCI interface\n");
        IExec->CloseLibrary(ExpansionBase);
        return 20;
    }

    /* Find the DMA controller */
    struct PCIDevice *dma_dev = IPCI->FindDeviceTags(
        FDT_VendorID, PASEMI_VENDOR_ID,
        FDT_DeviceID, PASEMI_DEV_DMA,
        TAG_DONE);

    if (!dma_dev) {
        IExec->DebugPrintF("DMA controller (0x1959:0xa007) not found!\n");
        IExec->DropInterface((struct Interface *)IPCI);
        IExec->CloseLibrary(ExpansionBase);
        return 20;
    }

    IExec->DebugPrintF("\n");
    IExec->DebugPrintF("PA6T-1682M ENVOI DMA Engine -- X1000 (Nemo)\n");
    IExec->DebugPrintF("============================================\n");

    /* Read capabilities */
    uint32 cap_ifi = dma_dev->ReadConfigLong(PAS_DMA_CAP_IFI);
    int32  nintf = (int32)((cap_ifi & PAS_DMA_CAP_IFI_NIN_M) >> PAS_DMA_CAP_IFI_NIN_S);
    int32  off   = (int32)((cap_ifi & PAS_DMA_CAP_IFI_IOFF_M) >> PAS_DMA_CAP_IFI_IOFF_S);

    uint32 cap_txch = dma_dev->ReadConfigLong(PAS_DMA_CAP_TXCH);
    int32  num_txch = (int32)((cap_txch & PAS_DMA_CAP_TXCH_TCHN_M) >> PAS_DMA_CAP_TXCH_TCHN_S);

    uint32 cap_rxch = dma_dev->ReadConfigLong(PAS_DMA_CAP_RXCH);
    int32  num_rxch = (int32)((cap_rxch & PAS_DMA_CAP_RXCH_RCHN_M) >> PAS_DMA_CAP_RXCH_RCHN_S);

    IExec->DebugPrintF("\nCapabilities:\n");
    IExec->DebugPrintF("  DMA interfaces: %ld\n", (long)nintf);
    IExec->DebugPrintF("  TX channels:    %ld\n", (long)num_txch);
    IExec->DebugPrintF("  RX channels:    %ld\n", (long)num_rxch);

    /* ---- 1. DMA Interface Table ---- */

    IExec->DebugPrintF("\nDMA Interface Table (offset 0x%04lX):\n", (long)off);
    IExec->DebugPrintF("  Each entry maps a SerDes lane to a DMA interface.\n");
    IExec->DebugPrintF("  On X1000, only interface 5 (SGMII) is active for Ethernet.\n\n");

    for (i = 0; i < (nintf + 3) / 4; i++) {
        uint32 entry = dma_dev->ReadConfigLong((uint32)off + (uint32)(4 * i));
        for (j = 0; j < 4; j++) {
            int32 intf_num = i * 4 + j;
            if (intf_num >= nintf) break;

            uint8 devfn = (uint8)((entry >> (8 * j)) & 0xff);
            uint8 dev = (devfn >> 3) & 0x1f;
            uint8 fn  = devfn & 0x07;

            if (devfn == 0) {
                IExec->DebugPrintF("  Interface %2ld: devfn=0x%02lX  -- (empty)\n",
                                   (long)intf_num, (uint32)devfn);
            } else {
                const char *hw   = serdes_hw_name(devfn);
                const char *use  = serdes_x1000_use(devfn);
                IExec->DebugPrintF("  Interface %2ld: devfn=0x%02lX (dev=%ld fn=%ld)  %s => %s\n",
                                   (long)intf_num, (uint32)devfn,
                                   (long)dev, (long)fn, hw, use);
            }
        }
    }

    /* ---- 2. TX Channels ---- */

    IExec->DebugPrintF("\nTX Channels (send path):\n");
    for (i = 0; i < num_txch; i++) {
        uint32 sta    = dma_dev->ReadConfigLong(PAS_DMA_TXCHAN_TCMDSTA((uint32)i));
        uint32 base_l = dma_dev->ReadConfigLong(PAS_DMA_TXCHAN_BASEL((uint32)i));
        uint32 base_u = dma_dev->ReadConfigLong(PAS_DMA_TXCHAN_BASEU((uint32)i));

        const char *active  = (sta & PAS_DMA_TXCHAN_TCMDSTA_ACT) ? "ACTIVE" : "idle";
        const char *enabled = (sta & PAS_DMA_TXCHAN_TCMDSTA_EN)  ? "EN" : "--";

        IExec->DebugPrintF("  TX chan %2ld: sta=0x%08lX [%s %s]  ring=0x%08lX (baseu=0x%08lX)\n",
                           (long)i, sta, active, enabled, base_l, base_u);
    }

    /* ---- 3. RX Channels ---- */

    IExec->DebugPrintF("\nRX Channels (receive path):\n");
    for (i = 0; i < num_rxch; i++) {
        uint32 sta    = dma_dev->ReadConfigLong(PAS_DMA_RXCHAN_CCMDSTA((uint32)i));
        uint32 base_l = dma_dev->ReadConfigLong(PAS_DMA_RXCHAN_BASEL((uint32)i));
        uint32 base_u = dma_dev->ReadConfigLong(PAS_DMA_RXCHAN_BASEU((uint32)i));

        const char *active  = (sta & PAS_DMA_RXCHAN_CCMDSTA_ACT) ? "ACTIVE" : "idle";
        const char *enabled = (sta & PAS_DMA_RXCHAN_CCMDSTA_EN)  ? "EN" : "--";

        IExec->DebugPrintF("  RX chan %2ld: sta=0x%08lX [%s %s]  ring=0x%08lX (baseu=0x%08lX)\n",
                           (long)i, sta, active, enabled, base_l, base_u);
    }

    IExec->DebugPrintF("\n");

    IPCI->FreeDevice(dma_dev);
    IExec->DropInterface((struct Interface *)IPCI);
    IExec->CloseLibrary(ExpansionBase);
    return 0;
}
