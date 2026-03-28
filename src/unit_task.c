/*
 * unit_task.c  -- unit task body for pa6t_eth.device.
 *
 * The unit task owns all blocking hardware access.  It waits on two
 * signal sources:
 *
 *   io_port    -- incoming IOSana2Req messages (S2_ONLINE, S2_OFFLINE, etc.)
 *   irq_mask   -- fired by the interrupt handler on every RX completion
 *   CTRL_C     -- set by UnitTask_Shutdown() to request orderly exit
 *
 * On S2_ONLINE / S2_CONFIGINTERFACE the task calls PA6TEth_PostRxBuffers
 * to arm the DMA RX ring and enable the MAC.  On every IRQ wakeup
 * ProcessRX is called to deliver received frames to their CMD_READ waiters.
 */

#include "pa6t_eth.h"
#include <exec/exec.h>
#include <exec/errors.h>
#include <stddef.h>

/* ---- Startup handshake message -------------------------------------------- */

struct UnitTaskStartMsg
{
    struct PA6TEthBase *base;
    struct PA6TEthUnit *unit;
    struct Task        *parent_task;
    uint32              ready_mask;
    int8                ready_bit;
};

/* ---- Signal event waiters ------------------------------------------------- */

static void signal_event(struct PA6TEthBase *base, struct PA6TEthUnit *unit,
                         ULONG event_mask)
{
    struct ExecIFace *IExec = base->IExec;

    IExec->Forbid();
    struct Node *n = unit->event_list.lh_Head;
    while (n->ln_Succ) {
        struct Node *next = n->ln_Succ;
        struct IOSana2Req *ev = (struct IOSana2Req *)
            ((uint8 *)n - offsetof(struct IORequest, io_Message.mn_Node));
        if (ev->ios2_WireError & event_mask) {
            IExec->Remove(n);
            ev->ios2_Req.io_Error = 0;
            ev->ios2_WireError    = event_mask & ev->ios2_WireError;
            IExec->ReplyMsg((struct Message *)ev);
        }
        n = next;
    }
    IExec->Permit();
}

/* ---- Unit task body ------------------------------------------------------- */

static void UnitTaskBody(void)
{
    struct Task *self = ((struct ExecIFace *)
        ((*(struct ExecBase **)4)->MainInterface))->FindTask(NULL);
    struct UnitTaskStartMsg *startMsg =
        (struct UnitTaskStartMsg *)self->tc_UserData;

    struct PA6TEthBase *base  = startMsg->base;
    struct PA6TEthUnit *unit  = startMsg->unit;
    struct ExecIFace   *IExec = base->IExec;

    DPRINTF(IExec, "[pa6t_eth] UnitTask: started\n");

    /* Allocate message port for incoming IORequests */
    unit->io_port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    if (!unit->io_port) {
        IExec->DebugPrintF("[pa6t_eth] UnitTask: AllocSysObject(PORT) failed\n");
        IExec->Signal(startMsg->parent_task, startMsg->ready_mask);
        return;
    }

    uint32 port_mask = 1UL << unit->io_port->mp_SigBit;
    unit->io_port_mask = port_mask;
    unit->task = self;

    /* Allocate signal bit for IRQ wakeup */
    int8 irq_bit = IExec->AllocSignal(-1);
    if (irq_bit < 0) {
        IExec->DebugPrintF("[pa6t_eth] UnitTask: AllocSignal failed\n");
        IExec->FreeSysObject(ASOT_PORT, unit->io_port);
        unit->io_port      = NULL;
        unit->io_port_mask = 0;
        unit->task         = NULL;
        IExec->Signal(startMsg->parent_task, startMsg->ready_mask);
        return;
    }

    uint32 irq_mask = 1UL << irq_bit;
    unit->irq_wait_task   = self;
    unit->irq_signal_mask = irq_mask;

    /* Signal parent: port is ready, safe to PutMsg now */
    IExec->Signal(startMsg->parent_task, startMsg->ready_mask);
    /* startMsg is stack-allocated  -- do NOT touch it after this point */

    uint32 wait_mask = port_mask | irq_mask | SIGBREAKF_CTRL_C;

    DPRINTF(IExec, "[pa6t_eth] UnitTask: entering main loop"
                   " (port_mask=0x%08lX irq_mask=0x%08lX)\n",
                   port_mask, irq_mask);

    while (!unit->task_shutdown) {
        unit->loop_count++;
        (void)IExec->Wait(wait_mask);

        /* Consume any additional IRQ signals that arrived during processing */
        IExec->SetSignal(0, irq_mask);

        if (IExec->SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
            break;

        /* Drain all pending commands from the IO port */
        struct Message *msg;
        while ((msg = IExec->GetMsg(unit->io_port)) != NULL) {
            struct IOSana2Req *ioreq = (struct IOSana2Req *)msg;

            switch (ioreq->ios2_Req.io_Command)
            {
            case S2_CONFIGINTERFACE:
                /*
                 * Roadshow and AmiTCP use S2_CONFIGINTERFACE to bring
                 * the interface online.  Accept re-configuration silently
                 * if already online (DHCP may call it again).
                 */
                DPRINTF(IExec, "[pa6t_eth] UnitTask: S2_CONFIGINTERFACE"
                        " (online=%d)\n", (int)unit->online);
                if (!unit->online) {
                    unit->online = TRUE;
                    PA6TEth_PostRxBuffers(base, unit);
                    signal_event(base, unit, S2EVENT_ONLINE);
                }
                ioreq->ios2_Req.io_Error = 0;
                ioreq->ios2_WireError    = 0;
                IExec->ReplyMsg(msg);
                break;

            case S2_ONLINE:
                DPRINTF(IExec, "[pa6t_eth] UnitTask: S2_ONLINE\n");
                if (!unit->online) {
                    unit->online = TRUE;
                    PA6TEth_PostRxBuffers(base, unit);
                    signal_event(base, unit, S2EVENT_ONLINE);
                }
                ioreq->ios2_Req.io_Error = 0;
                ioreq->ios2_WireError    = 0;
                IExec->ReplyMsg(msg);
                break;

            case S2_OFFLINE:
                DPRINTF(IExec, "[pa6t_eth] UnitTask: S2_OFFLINE\n");
                unit->online = FALSE;
                PA6TEth_FlushReadQueue(base, unit, S2ERR_OUTOFSERVICE);
                signal_event(base, unit, S2EVENT_OFFLINE);
                ioreq->ios2_Req.io_Error = 0;
                ioreq->ios2_WireError    = 0;
                IExec->ReplyMsg(msg);
                break;

            case CMD_FLUSH:
                PA6TEth_FlushReadQueue(base, unit, IOERR_ABORTED);
                ioreq->ios2_Req.io_Error = 0;
                IExec->ReplyMsg(msg);
                break;

            /* Write commands are handled inline in BeginIO  -- if one
             * somehow reaches here, abort it gracefully */
            case CMD_WRITE:
            case S2_BROADCAST:
            case S2_MULTICAST:
                IExec->DebugPrintF("[pa6t_eth] UnitTask: unexpected write cmd"
                                   " %u  -- aborting\n",
                                   (uint32)ioreq->ios2_Req.io_Command);
                ioreq->ios2_Req.io_Error = S2ERR_TX_FAILURE;
                ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
                IExec->ReplyMsg(msg);
                break;

            default:
                ioreq->ios2_Req.io_Error = IOERR_NOCMD;
                IExec->ReplyMsg(msg);
                break;
            }
        }

        /* Process any RX completions signalled by the IRQ handler */
        if (unit->online)
            PA6TEth_ProcessRX(base, unit);

    }

    DPRINTF(IExec, "[pa6t_eth] UnitTask: exited main loop"
                   " (shutdown=%ld)\n", (long)unit->task_shutdown);

    /* Drain remaining messages before exit */
    {
        struct Message *m;
        while ((m = IExec->GetMsg(unit->io_port)) != NULL) {
            struct IOSana2Req *ioreq = (struct IOSana2Req *)m;
            ioreq->ios2_Req.io_Error = IOERR_ABORTED;
            IExec->ReplyMsg(m);
        }
    }

    /* Abort all pending read requests and event subscribers so callers
     * (RoadShow, Media Toolbox, Ranger, etc.) are not left hanging. */
    PA6TEth_FlushAllPending(base, unit);

    unit->irq_wait_task   = NULL;
    unit->irq_signal_mask = 0;
    IExec->FreeSignal(irq_bit);

    IExec->FreeSysObject(ASOT_PORT, unit->io_port);
    unit->io_port      = NULL;
    unit->io_port_mask = 0;
    unit->task         = NULL;

    DPRINTF(IExec, "[pa6t_eth] UnitTask: exiting\n");
}

/* ---- Start / Shutdown ----------------------------------------------------- */

BOOL UnitTask_Start(struct PA6TEthBase *base, struct PA6TEthUnit *unit)
{
    struct ExecIFace *IExec = base->IExec;

    int8 ready_bit = IExec->AllocSignal(-1);
    if (ready_bit < 0) {
        DPRINTF(IExec, "[pa6t_eth] UnitTask_Start: AllocSignal failed\n");
        return FALSE;
    }

    struct UnitTaskStartMsg startMsg;
    startMsg.base        = base;
    startMsg.unit        = unit;
    startMsg.parent_task = IExec->FindTask(NULL);
    startMsg.ready_mask  = 1UL << ready_bit;
    startMsg.ready_bit   = ready_bit;

    unit->task_shutdown = FALSE;

    /* Create task under Forbid so tc_UserData is set before it runs */
    IExec->Forbid();
    struct Task *task = IExec->CreateTaskTags("pa6t-eth unit task", 50,
                                              UnitTaskBody, 32768,
                                              TAG_DONE);
    if (task)
        task->tc_UserData = (APTR)&startMsg;
    IExec->Permit();

    if (!task) {
        DPRINTF(IExec, "[pa6t_eth] UnitTask_Start: CreateTaskTags failed\n");
        IExec->FreeSignal(ready_bit);
        return FALSE;
    }

    /* Wait for task to open its port (or fail) */
    IExec->Wait(startMsg.ready_mask);
    IExec->FreeSignal(ready_bit);

    if (!unit->io_port) {
        DPRINTF(IExec, "[pa6t_eth] UnitTask_Start: task failed to init\n");
        return FALSE;
    }

    DPRINTF(IExec, "[pa6t_eth] UnitTask_Start: task started OK\n");
    return TRUE;
}

void UnitTask_Shutdown(struct PA6TEthBase *base, struct PA6TEthUnit *unit)
{
    struct ExecIFace *IExec = base->IExec;

    if (!unit->task) return;

    DPRINTF(IExec, "[pa6t_eth] UnitTask_Shutdown\n");

    unit->task_shutdown = TRUE;
    struct Task *t = unit->task;
    IExec->Signal(t, SIGBREAKF_CTRL_C);

    /* Yield until the task clears unit->task */
    uint32 patience = 100000;
    while (unit->task && patience-- > 0) {
        IExec->Forbid();
        IExec->Permit();
    }

    DPRINTF(IExec, "[pa6t_eth] UnitTask_Shutdown: done\n");
}
