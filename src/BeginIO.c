/*
 * BeginIO.c  -- SANA-II command dispatcher for pa6t_eth.device.
 *
 * CMD_READ is queued to the read_list (replied later by ProcessRX).
 * CMD_WRITE / S2_BROADCAST / S2_MULTICAST are processed inline in the
 * caller's task context.
 * Commands that need hardware access (S2_ONLINE, S2_OFFLINE, etc.) are
 * queued to the unit task message port.
 */

#include "pa6t_eth.h"
#include <exec/errors.h>
#include <stddef.h>

/* ---- helpers --------------------------------------------------------------- */

static void reply_ioreq(struct ExecIFace *IExec, struct IOSana2Req *ioreq,
                        BYTE err, ULONG werr)
{
    ioreq->ios2_Req.io_Error = err;
    ioreq->ios2_WireError    = werr;
    if (ioreq->ios2_Req.io_Flags & IOF_QUICK)
        return;
    IExec->ReplyMsg((struct Message *)ioreq);
}

static void queue_to_task(struct ExecIFace *IExec, struct PA6TEthUnit *unit,
                          struct IOSana2Req *ioreq)
{
    ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
    IExec->PutMsg(unit->io_port, (struct Message *)ioreq);
}

/* ---- BeginIO --------------------------------------------------------------- */

void _manager_BeginIO(struct DeviceManagerInterface *Self,
                      struct IOSana2Req *ioreq)
{
    struct PA6TEthBase *base  = (struct PA6TEthBase *)Self->Data.LibBase;
    struct ExecIFace   *IExec = base->IExec;
    struct PA6TEthUnit *unit  = (struct PA6TEthUnit *)ioreq->ios2_Req.io_Unit;

    ioreq->ios2_Req.io_Error = 0;
    ioreq->ios2_WireError    = 0;

    DPRINTF(IExec, "[pa6t_eth] BeginIO: cmd %u unit_online=%d\n",
            (uint32)ioreq->ios2_Req.io_Command, (int)unit->online);

    switch (ioreq->ios2_Req.io_Command)
    {
    /* S2_DEVICEQUERY */
    case S2_DEVICEQUERY:
    {
        struct Sana2DeviceQuery *q =
            (struct Sana2DeviceQuery *)ioreq->ios2_StatData;
        if (!q) {
            reply_ioreq(IExec, ioreq, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
            return;
        }
        ULONG avail  = q->SizeAvailable;
        ULONG supply = sizeof(struct Sana2DeviceQuery);
        if (supply > avail) supply = avail;
        q->SizeSupplied   = supply;
        q->DevQueryFormat = 0;
        q->DeviceLevel    = 0;
        if (supply >= offsetof(struct Sana2DeviceQuery, AddrFieldSize) + sizeof(UWORD))
            q->AddrFieldSize = ETH_ADDR_LEN * 8; /* 48 bits */
        if (supply >= offsetof(struct Sana2DeviceQuery, MTU) + sizeof(ULONG))
            q->MTU = 1500;
        if (supply >= offsetof(struct Sana2DeviceQuery, BPS) + sizeof(ULONG))
            q->BPS = 1000000000UL;  /* 1 Gbps */
        if (supply >= offsetof(struct Sana2DeviceQuery, HardwareType) + sizeof(ULONG))
            q->HardwareType = S2WireType_Ethernet;
        if (supply >= offsetof(struct Sana2DeviceQuery, RawMTU) + sizeof(ULONG))
            q->RawMTU = ETH_MAX_FRAME;
        reply_ioreq(IExec, ioreq, 0, 0);
        return;
    }

    /* S2_GETSTATIONADDRESS */
    case S2_GETSTATIONADDRESS:
        IExec->CopyMem(base->mac_addr, ioreq->ios2_SrcAddr, ETH_ADDR_LEN);
        IExec->CopyMem(base->mac_addr, ioreq->ios2_DstAddr, ETH_ADDR_LEN);
        reply_ioreq(IExec, ioreq, 0, 0);
        return;

    /* S2_CONFIGINTERFACE */
    case S2_CONFIGINTERFACE:
        DPRINTF(IExec, "[pa6t_eth] S2_CONFIGINTERFACE: unit_online=%d -> queuing\n",
                (int)unit->online);
        queue_to_task(IExec, unit, ioreq);
        return;

    /* S2_ONLINE / S2_OFFLINE */
    case S2_ONLINE:
        if (unit->online) {
            reply_ioreq(IExec, ioreq, S2ERR_BAD_STATE, S2WERR_UNIT_ONLINE);
            return;
        }
        queue_to_task(IExec, unit, ioreq);
        return;

    case S2_OFFLINE:
        if (!unit->online) {
            reply_ioreq(IExec, ioreq, S2ERR_BAD_STATE, S2WERR_UNIT_OFFLINE);
            return;
        }
        queue_to_task(IExec, unit, ioreq);
        return;

    /* CMD_READ */
    case CMD_READ:
    {
        if (!unit->online) {
            reply_ioreq(IExec, ioreq, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
            return;
        }
        if (!PA6TEth_FindOpener(unit, ioreq->ios2_BufferManagement)) {
            IExec->DebugPrintF("[pa6t_eth] CMD_READ: no opener for bm=%p\n",
                               ioreq->ios2_BufferManagement);
            reply_ioreq(IExec, ioreq, S2ERR_BAD_STATE, S2WERR_GENERIC_ERROR);
            return;
        }
        /* Allocate a PendingRead wrapper (avoids touching ioreq's mn_Node) */
        struct PendingRead *pr = (struct PendingRead *)IExec->AllocVecTags(
            sizeof(struct PendingRead),
            AVT_Type, MEMF_SHARED,
            TAG_DONE);
        if (!pr) {
            reply_ioreq(IExec, ioreq, S2ERR_NO_RESOURCES, S2WERR_GENERIC_ERROR);
            return;
        }
        pr->ioreq = ioreq;
        DPRINTF(IExec, "[pa6t_eth] CMD_READ: queuing EtherType 0x%04lX cookie=%p\n",
                ioreq->ios2_PacketType, ioreq->ios2_Data);
        ioreq->ios2_StatData = ioreq->ios2_Data;    /* save real 68k cookie */
        ioreq->ios2_Data     = (APTR)ioreq;          /* self-reference       */
        ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
        struct List *rlist;
        switch ((uint16)ioreq->ios2_PacketType) {
        case 0x0800: rlist = &unit->read_list_ipv4;    break;
        case 0x0806: rlist = &unit->read_list_arp;   break;
        default:     rlist = &unit->read_list_other; break;
        }
        IExec->Forbid();
        IExec->AddTail(rlist, (struct Node *)&pr->node);
        IExec->Permit();
        return; /* replied later by ProcessRX */
    }

    /* S2_READORPHAN */
    case S2_READORPHAN:
    {
        if (!PA6TEth_FindOpener(unit, ioreq->ios2_BufferManagement)) {
            IExec->DebugPrintF("[pa6t_eth] S2_READORPHAN: no opener for bm=%p\n",
                               ioreq->ios2_BufferManagement);
            reply_ioreq(IExec, ioreq, S2ERR_BAD_STATE, S2WERR_GENERIC_ERROR);
            return;
        }
        DPRINTF(IExec, "[pa6t_eth] S2_READORPHAN: queuing cookie=%p\n",
                ioreq->ios2_Data);
        ioreq->ios2_StatData = ioreq->ios2_Data;
        ioreq->ios2_Data     = (APTR)ioreq;
        ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
        IExec->Forbid();
        IExec->AddTail(&unit->event_list,
                       (struct Node *)&ioreq->ios2_Req.io_Message.mn_Node);
        IExec->Permit();
        return;
    }

    /* CMD_WRITE / S2_BROADCAST / S2_MULTICAST */
    case CMD_WRITE:
    case S2_BROADCAST:
    case S2_MULTICAST:
        if (!unit->online) {
            reply_ioreq(IExec, ioreq, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
            return;
        }
        if (ioreq->ios2_Req.io_Command == S2_BROADCAST) {
            uint8 *dst = ioreq->ios2_DstAddr;
            dst[0] = dst[1] = dst[2] = dst[3] = dst[4] = dst[5] = 0xFF;
        }
        DPRINTF(IExec, "[pa6t_eth] %s: type=0x%04lX dst=%02X:%02X:%02X:%02X:%02X:%02X"
                " len=%lu cookie=%p\n",
                (ioreq->ios2_Req.io_Command == S2_BROADCAST) ? "S2_BROADCAST" :
                (ioreq->ios2_Req.io_Command == S2_MULTICAST) ? "S2_MULTICAST" :
                "CMD_WRITE",
                ioreq->ios2_PacketType,
                (uint32)ioreq->ios2_DstAddr[0], (uint32)ioreq->ios2_DstAddr[1],
                (uint32)ioreq->ios2_DstAddr[2], (uint32)ioreq->ios2_DstAddr[3],
                (uint32)ioreq->ios2_DstAddr[4], (uint32)ioreq->ios2_DstAddr[5],
                ioreq->ios2_DataLength, ioreq->ios2_Data);
        ioreq->ios2_StatData = ioreq->ios2_Data;
        ioreq->ios2_Data     = (APTR)ioreq;
        PA6TEth_TransmitPacket(base, unit, ioreq);
        return;

    /* CMD_FLUSH */
    case CMD_FLUSH:
        queue_to_task(IExec, unit, ioreq);
        return;

    /* S2_ONEVENT */
    case S2_ONEVENT:
        ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
        IExec->Forbid();
        IExec->AddTail(&unit->event_list,
                       (struct Node *)&ioreq->ios2_Req.io_Message.mn_Node);
        IExec->Permit();
        return;

    /* S2_GETGLOBALSTATS */
    case S2_GETGLOBALSTATS:
    {
        struct Sana2DeviceStats *s =
            (struct Sana2DeviceStats *)ioreq->ios2_StatData;
        if (!s) {
            reply_ioreq(IExec, ioreq, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
            return;
        }
        s->PacketsReceived      = unit->stat_rx;
        s->PacketsSent          = unit->stat_tx;
        s->BadData              = 0;
        s->Overruns             = 0;
        s->UnknownTypesReceived = unit->stat_rx_dropped;
        s->Reconfigurations     = 0;
        reply_ioreq(IExec, ioreq, 0, 0);
        return;
    }

    /* S2_ADDMULTICASTADDRESS / S2_DELMULTICASTADDRESS (stub) */
    case S2_ADDMULTICASTADDRESS:
    case S2_DELMULTICASTADDRESSES:
    case S2_DELMULTICASTADDRESS:
    case S2_ADDMULTICASTADDRESSES:
        reply_ioreq(IExec, ioreq, 0, 0);
        return;

    /* S2_TRACKTYPE / S2_UNTRACKTYPE (stub) */
    case S2_TRACKTYPE:
    case S2_UNTRACKTYPE:
        reply_ioreq(IExec, ioreq, 0, 0);
        return;

    /* S2_SANA2HOOK -- install native PPC copy hook */
    case S2_SANA2HOOK:
    {
        struct Opener *op = PA6TEth_FindOpener(unit, ioreq->ios2_BufferManagement);
        if (!op) {
            reply_ioreq(IExec, ioreq, S2ERR_BAD_STATE, S2WERR_GENERIC_ERROR);
            return;
        }
        op->s2hook = (struct Sana2Hook *)ioreq->ios2_Data;
        DPRINTF(IExec, "[pa6t_eth] S2_SANA2HOOK: native PPC hook=%p installed"
                       " for opener=%p\n",
                       op->s2hook, op);
        reply_ioreq(IExec, ioreq, 0, 0);
        return;
    }

    /* Unsupported */
    default:
        IExec->DebugPrintF("[pa6t_eth] BeginIO: unknown cmd %lu (0x%04lX) "
                           " -- IOERR_NOCMD\n",
                           (uint32)ioreq->ios2_Req.io_Command,
                           (uint32)ioreq->ios2_Req.io_Command);
        reply_ioreq(IExec, ioreq, IOERR_NOCMD, S2WERR_GENERIC_ERROR);
        return;
    }
}

/* ---- AbortIO --------------------------------------------------------------- */

LONG _manager_AbortIO(struct DeviceManagerInterface *Self,
                      struct IOSana2Req *ioreq)
{
    struct PA6TEthBase *base  = (struct PA6TEthBase *)Self->Data.LibBase;
    struct ExecIFace   *IExec = base->IExec;
    struct PA6TEthUnit *unit  = (struct PA6TEthUnit *)ioreq->ios2_Req.io_Unit;

    if (!unit) return IOERR_NOCMD;

    /* Search all per-type read lists for the PendingRead wrapping this ioreq */
    IExec->Forbid();
    struct List *read_lists[3] = {
        &unit->read_list_ipv4,
        &unit->read_list_arp,
        &unit->read_list_other
    };
    BOOL found = FALSE;
    for (int li = 0; li < 3 && !found; li++) {
        struct Node *n = read_lists[li]->lh_Head;
        while (n->ln_Succ) {
            struct PendingRead *pr = (struct PendingRead *)n;
            if (pr->ioreq == ioreq) {
                IExec->Remove(n);
                IExec->FreeVec(pr);
                found = TRUE;
                break;
            }
            n = n->ln_Succ;
        }
    }
    IExec->Permit();

    if (found) {
        ioreq->ios2_Req.io_Error = IOERR_ABORTED;
        IExec->ReplyMsg((struct Message *)ioreq);
        return 0;
    }

    /* Try the event_list (S2_READORPHAN, S2_ONEVENT) */
    IExec->Forbid();
    {
        struct Node *en = unit->event_list.lh_Head;
        while (en->ln_Succ) {
            if (en == (struct Node *)&ioreq->ios2_Req.io_Message.mn_Node) {
                IExec->Remove(en);
                found = TRUE;
                break;
            }
            en = en->ln_Succ;
        }
    }
    IExec->Permit();

    if (found) {
        ioreq->ios2_Req.io_Error = IOERR_ABORTED;
        IExec->ReplyMsg((struct Message *)ioreq);
        return 0;
    }

    /* Try the unit task's message port */
    if (unit->io_port) {
        IExec->Forbid();
        struct Message *msg = (struct Message *)unit->io_port->mp_MsgList.lh_Head;
        found = FALSE;
        while (msg->mn_Node.ln_Succ) {
            if (msg == (struct Message *)ioreq) {
                IExec->Remove((struct Node *)msg);
                found = TRUE;
                break;
            }
            msg = (struct Message *)msg->mn_Node.ln_Succ;
        }
        IExec->Permit();
        if (found) {
            ioreq->ios2_Req.io_Error = IOERR_ABORTED;
            IExec->ReplyMsg((struct Message *)ioreq);
            return 0;
        }
    }

    return IOERR_NOCMD;
}
