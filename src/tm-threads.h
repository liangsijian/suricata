/* Copyright (C) 2007-2011 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 */

#ifndef __TM_THREADS_H__
#define __TM_THREADS_H__

#include "tmqh-packetpool.h"
#include "tm-threads-common.h"
#include "tm-modules.h"

typedef struct TmSlot_ {
    /* the TV holding this slot */
    ThreadVars *tv;

    /* function pointers */
	// 包处理函数
    TmEcode (*SlotFunc)(ThreadVars *, Packet *, void *, PacketQueue *,
                        PacketQueue *);

    TmEcode (*PktAcqLoop)(ThreadVars *, void *, void *);

    TmEcode (*SlotThreadInit)(ThreadVars *, void *, void **); // tm_func 被调用初始化模块
    void (*SlotThreadExitPrintStats)(ThreadVars *, void *);
    TmEcode (*SlotThreadDeinit)(ThreadVars *, void *);

    /* data storage */
    void *slot_initdata; // 传递给 SlotThread* 函数的第二参数
    SC_ATOMIC_DECLARE(void *, slot_data); // 传递给 SlotThread* 函数的第三参数

    /* queue filled by the SlotFunc with packets that will
     * be processed futher _before_ the current packet.
     * The locks in the queue are NOT used */
    PacketQueue slot_pre_pq; // 在当前包放入 tv->tmqh_out 之前被处理的包

    /* queue filled by the SlotFunc with packets that will
     * be processed futher _after_ the current packet. The
     * locks in the queue are NOT used */
    PacketQueue slot_post_pq;

    /* store the thread module id */
    int tm_id;

    /* slot id, only used my TmVarSlot to know what the first slot is */
    int id;

    /* linked list, only used when you have multiple slots(used by TmVarSlot) */
    struct TmSlot_ *slot_next;
} TmSlot;

extern ThreadVars *tv_root[TVT_MAX];

extern SCMutex tv_root_lock;

void TmSlotSetFuncAppend(ThreadVars *, TmModule *, void *);
TmSlot *TmSlotGetSlotForTM(int);

ThreadVars *TmThreadCreate(char *, char *, char *, char *, char *, char *,
                           void *(fn_p)(void *), int);
ThreadVars *TmThreadCreatePacketHandler(char *, char *, char *, char *, char *,
                                        char *);
ThreadVars *TmThreadCreateMgmtThread(char *name, void *(fn_p)(void *), int);
TmEcode TmThreadSpawn(ThreadVars *);
void TmThreadSetFlags(ThreadVars *, uint8_t);
void TmThreadSetAOF(ThreadVars *, uint8_t);
void TmThreadKillThread(ThreadVars *);
void TmThreadKillThreads(void);
void TmThreadAppend(ThreadVars *, int);
void TmThreadRemove(ThreadVars *, int);

TmEcode TmThreadSetCPUAffinity(ThreadVars *, uint16_t);
TmEcode TmThreadSetThreadPriority(ThreadVars *, int);
TmEcode TmThreadSetCPU(ThreadVars *, uint8_t);
TmEcode TmThreadSetupOptions(ThreadVars *);
void TmThreadSetPrio(ThreadVars *);
int TmThreadGetNbThreads(uint8_t type);

void TmThreadInitMC(ThreadVars *);
void TmThreadTestThreadUnPaused(ThreadVars *);
void TmThreadContinue(ThreadVars *);
void TmThreadContinueThreads(void);
void TmThreadPause(ThreadVars *);
void TmThreadPauseThreads(void);
void TmThreadCheckThreadState(void);
TmEcode TmThreadWaitOnThreadInit(void);
ThreadVars *TmThreadsGetCallingThread(void);

int TmThreadsCheckFlag(ThreadVars *, uint8_t);
void TmThreadsSetFlag(ThreadVars *, uint8_t);
void TmThreadsUnsetFlag(ThreadVars *, uint8_t);
void TmThreadWaitForFlag(ThreadVars *, uint8_t);

TmEcode TmThreadsSlotVarRun (ThreadVars *tv, Packet *p, TmSlot *slot);

ThreadVars *TmThreadsGetTVContainingSlot(TmSlot *);
void TmThreadDisableReceiveThreads(void);
void TmThreadDisableUptoDetectThreads(void);
TmSlot *TmThreadGetFirstTmSlotForPartialPattern(const char *);

/**
 *  \brief Process the rest of the functions (if any) and queue.
 */
static inline TmEcode TmThreadsSlotProcessPkt(ThreadVars *tv, TmSlot *s, Packet *p)
{
    TmEcode r = TM_ECODE_OK;

    if (s == NULL) {
        tv->tmqh_out(tv, p);
        return r;
    }
	// 如果处理当前包 p 之后，有往s->slot_pre_pq中添加包，会预先递归调用后续的
	// s->SlotFunc处理，并在 p 放入 tv->tmqh_out 之前将先将 s->slot_pre_pq 处理
	// 的结果入队列
    if (TmThreadsSlotVarRun(tv, p, s) == TM_ECODE_FAILED) {
        TmqhOutputPacketpool(tv, p);
        TmSlot *slot = s;
        while (slot != NULL) {
            SCMutexLock(&slot->slot_post_pq.mutex_q);
            TmqhReleasePacketsToPacketPool(&slot->slot_post_pq);
            SCMutexUnlock(&slot->slot_post_pq.mutex_q);

            slot = slot->slot_next;
        }
        TmThreadsSetFlag(tv, THV_FAILED);
        r = TM_ECODE_FAILED;

    } else {
		// 当前包处理完之后放入输出队列
        tv->tmqh_out(tv, p);

        /* post process pq */
        TmSlot *slot = s;
        while (slot != NULL) {
			// 有后置包处理，在 p 放入 tmqh_out 之后在将后置包放入
            if (slot->slot_post_pq.top != NULL) {
                while (1) {
                    SCMutexLock(&slot->slot_post_pq.mutex_q);
                    Packet *extra_p = PacketDequeue(&slot->slot_post_pq);
                    SCMutexUnlock(&slot->slot_post_pq.mutex_q);

                    if (extra_p == NULL)
                        break;

                    if (slot->slot_next != NULL) {
                        r = TmThreadsSlotVarRun(tv, extra_p, slot->slot_next);
                        if (r == TM_ECODE_FAILED) {
                            SCMutexLock(&slot->slot_post_pq.mutex_q);
                            TmqhReleasePacketsToPacketPool(&slot->slot_post_pq);
                            SCMutexUnlock(&slot->slot_post_pq.mutex_q);

                            TmqhOutputPacketpool(tv, extra_p);
                            TmThreadsSetFlag(tv, THV_FAILED);
                            break;
                        }
                    }
                    tv->tmqh_out(tv, extra_p);
                }
            } /* if (slot->slot_post_pq.top != NULL) */
            slot = slot->slot_next;
        } /* while (slot != NULL) */
    }

    return r;
}

#endif /* __TM_THREADS_H__ */
