/*
 * Copyright (c) 2025 The gem5 Contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/ruby/protocol/chi/c2c/C2CPacketizerBridge.hh"

#include "debug/RubyCHIC2CPacketizer.hh"
#include "mem/ruby/system/RubySystem.hh"

namespace gem5
{

namespace ruby
{

C2CPacketizerBridge::C2CPacketizerBridge(const Params &p)
    : ClockedObject(p),
      Consumer(this),
      containerLatency(Cycles(p.container_latency)),
      txqSize(p.txq_size),
      bufferedGranules(0),
      rubySystem(p.ruby_system),
      txCreditMgr(nullptr),
      rxCreditMgr(nullptr)
{
    inBuf[CH_RSP] = p.txRsp;
    inBuf[CH_DAT] = p.txDat;
    inBuf[CH_SNP] = p.txSnp;
    inBuf[CH_REQ] = p.txReq;
    inBuf[CH_MISC] = p.txMisc;

    outBuf[CH_RSP] = p.rxRsp;
    outBuf[CH_DAT] = p.rxDat;
    outBuf[CH_SNP] = p.rxSnp;
    outBuf[CH_REQ] = p.rxReq;
    outBuf[CH_MISC] = p.rxMisc;
}

void
C2CPacketizerBridge::init()
{
    ClockedObject::init();
    for (int i = 0; i < NUM_CHANNELS; i++) {
        inBuf[i]->setConsumer(this);
    }
}

void
C2CPacketizerBridge::startup()
{
    ClockedObject::startup();

    // Look up credit managers via the static registry.
    // This runs in startup() (after all init() calls) because the
    // SLICC-generated controller creates its C2CCreditManager in init().
    txCreditMgr = C2CCreditManager::lookup(params().tx_controller);
    rxCreditMgr = C2CCreditManager::lookup(params().rx_controller);
    panic_if(!txCreditMgr, "%s: no C2CCreditManager for tx_controller",
             name());
    panic_if(!rxCreditMgr, "%s: no C2CCreditManager for rx_controller",
             name());
}

void
C2CPacketizerBridge::print(std::ostream &out) const
{
    out << "[C2CPacketizerBridge " << name() << "]";
}

unsigned
C2CPacketizerBridge::granulesForChannel(int ch)
{
    // Upper-bound granule cost per channel (IHI0098A Table 4.4)
    using namespace chi_c2c;
    switch (ch) {
        case CH_RSP:
            return granulesForMsgType(MsgType::Resp);
        case CH_SNP:
            return granulesForMsgType(MsgType::Snoop);
        case CH_MISC:
            return granulesForMsgType(MsgType::MiscU);
        case CH_REQ:
            return granulesForMsgType(MsgType::ReqS);
        case CH_DAT:
            return granulesForMsgType(MsgType::DataL);
        default:
            panic("C2CPacketizerBridge: invalid channel %d", ch);
            return 0;
    }
}

void
C2CPacketizerBridge::wakeup()
{
    Tick curTk = curTick();

    for (int i = 0; i < NUM_CHANNELS; i++) {
        while (inBuf[i]->isReady(curTk)) {
            // Enforce TXQ limit: stop draining when buffer is full
            if (txqSize > 0 && bufferedGranules >= txqSize) {
                scheduleEvent(Cycles(1));
                break;
            }
            MsgPtr msg = inBuf[i]->peekMsgPtr();
            msgQueues[i].push_back(msg);
            bufferedGranules += granulesForChannel(i);
            inBuf[i]->dequeue(curTk);
        }
    }

    packAndDeliver();

    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (!msgQueues[i].empty()) {
            scheduleEvent(Cycles(1));
            return;
        }
    }
}

void
C2CPacketizerBridge::packAndDeliver()
{
    unsigned remaining = chi_c2c::NUM_MSG_GRANULES;
    Tick curTk = curTick();
    Tick delta = cyclesToTicks(containerLatency);

    for (int pi = 0; pi < NUM_CHANNELS; pi++) {
        int ch = priorityOrder[pi];
        unsigned granPerMsg = granulesForChannel(ch);

        while (!msgQueues[ch].empty() && granPerMsg <= remaining) {
            if (!outBuf[ch]->areNSlotsAvailable(1, curTk)) {
                break;
            }

            MsgPtr msg = msgQueues[ch].front();
            msgQueues[ch].pop_front();
            bufferedGranules -= granPerMsg;

            outBuf[ch]->enqueue(msg, curTk, delta,
                                rubySystem->getRandomization(),
                                rubySystem->getWarmupEnabled());

            remaining -= granPerMsg;

            DPRINTF(RubyCHIC2CPacketizer,
                    "Packed ch=%d, %u granules used, %u remain\n", ch,
                    granPerMsg, remaining);
        }
    }

    // Piggyback credit returns only when a container was actually sent
    if (remaining < chi_c2c::NUM_MSG_GRANULES) {
        uint8_t crReq = 0, crRsp = 0, crDat = 0, crSnp = 0;
        txCreditMgr->drainReturnPending(crReq, crRsp, crDat, crSnp);
        if (crReq || crRsp || crDat || crSnp) {
            rxCreditMgr->applyReturnCredits(crReq, crRsp, crDat, crSnp);
            DPRINTF(RubyCHIC2CPacketizer,
                    "Credit return: req=%u rsp=%u dat=%u snp=%u\n", crReq,
                    crRsp, crDat, crSnp);
        }
    }
}

} // namespace ruby
} // namespace gem5
