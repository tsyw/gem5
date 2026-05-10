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
#include "mem/ruby/protocol/CHI/CHIDataMsg.hh"
#include "mem/ruby/protocol/CHI/CHIDataType.hh"
#include "mem/ruby/protocol/CHI/CHIRequestMsg.hh"
#include "mem/ruby/protocol/CHI/CHIRequestType.hh"
#include "mem/ruby/protocol/CHI/CHIResponseMsg.hh"
#include "mem/ruby/protocol/CHI/CHI_C2C_MiscMsg.hh"
#include "mem/ruby/protocol/CHI/CHI_C2C_MiscMsgType.hh"
#include "mem/ruby/protocol/chi/c2c/C2CPacketizer.hh"
#include "mem/ruby/system/RubySystem.hh"

namespace gem5
{

namespace ruby
{

namespace
{

using CHI::CHI_C2C_MiscMsg;
using CHI::CHIDataMsg;
using CHI::CHIRequestMsg;
using CHI::CHIResponseMsg;
using chi_c2c::Channel;
using chi_c2c::ContainerPacker;
using chi_c2c::GRANULE_SIZE;
using chi_c2c::MsgType;
using chi_c2c::QueuedMsg;
using chi_c2c::wireBytesForMsgType;

enum BridgeChannelIdx
{
    BridgeRsp = 0,
    BridgeDat = 1,
    BridgeSnp = 2,
    BridgeReq = 3,
    BridgeMisc = 4,
};

Channel
toPacketizerChannel(int ch)
{
    switch (ch) {
        case BridgeRsp:
            return Channel::RSP;
        case BridgeDat:
            return Channel::DAT;
        case BridgeSnp:
            return Channel::SNP;
        case BridgeReq:
            return Channel::REQ;
        case BridgeMisc:
            return Channel::MISC;
        default:
            panic("C2CPacketizerBridge: invalid channel %d", ch);
    }
}

MsgType
requestMsgType(const CHIRequestMsg &msg, int ch)
{
    const auto type = msg.gettype();

    switch (ch) {
        case BridgeSnp:
            return MsgType::Snoop;
        case BridgeReq:
            switch (type) {
                case CHI::CHIRequestType_DvmTlbi_Initiate:
                case CHI::CHIRequestType_DvmSync_Initiate:
                case CHI::CHIRequestType_DvmSync_ExternCompleted:
                case CHI::CHIRequestType_SnpDvmOpSync_P1:
                case CHI::CHIRequestType_SnpDvmOpSync_P2:
                case CHI::CHIRequestType_SnpDvmOpNonSync_P1:
                case CHI::CHIRequestType_SnpDvmOpNonSync_P2:
                case CHI::CHIRequestType_DvmOpNonSync:
                case CHI::CHIRequestType_DvmOpSync:
                    return MsgType::ReqL;
                default:
                    return MsgType::ReqS;
            }
        default:
            panic("C2CPacketizerBridge: invalid request channel %d", ch);
    }
}

MsgType
responseMsgType(const CHIResponseMsg &msg)
{
    switch (msg.gettype()) {
        case CHI::CHIResponseType_DBIDResp:
        case CHI::CHIResponseType_CompDBIDResp:
        case CHI::CHIResponseType_RespSepData:
            return MsgType::Resp2;
        default:
            return MsgType::Resp;
    }
}

MsgType
dataMsgType(const CHIDataMsg &msg)
{
    const auto type = msg.gettype();

    switch (type) {
        case CHI::CHIDataType_WrReqDataS:
            return MsgType::WrReqS;
        case CHI::CHIDataType_WrReqDataL:
            return MsgType::WrReqL;
        default:
            break;
    }

    const auto validBytes = msg.getbitMask().count();
    panic_if(validBytes == 0,
             "C2CPacketizerBridge: data message without valid bytes");
    panic_if(validBytes > 64,
             "C2CPacketizerBridge: data message too large: %zu bytes",
             validBytes);

    return validBytes <= 32 ? MsgType::DataS : MsgType::DataL;
}

MsgType
miscMsgType(const CHI_C2C_MiscMsg &msg)
{
    switch (msg.gettype()) {
        case CHI::CHI_C2C_MiscMsgType_CreditGrant:
        case CHI::CHI_C2C_MiscMsgType_CreditReturn:
            return MsgType::MiscU;
        default:
            return MsgType::MiscC;
    }
}

QueuedMsg
buildQueuedMsgPayload(const CHIDataMsg &msg, MsgType type)
{
    QueuedMsg queued;
    queued.channel = Channel::DAT;
    queued.type = type;

    if (type == MsgType::DataS || type == MsgType::WrReqS) {
        const bool upperHalf = (msg.getaddr() & 0x20) != 0;
        const unsigned base = upperHalf ? 32 : 0;

        queued.shortDataUpper = upperHalf;
        queued.chunkValid = upperHalf ? 0x2 : 0x1;
        queued.data.resize(32, 0);

        for (unsigned idx = 0; idx < 32; ++idx) {
            const unsigned byte = base + idx;
            panic_if(
                msg.getbitMask().test(byte) &&
                    ((upperHalf && byte < 32) || (!upperHalf && byte >= 32)),
                "C2CPacketizerBridge: short data bitMask crosses addr[5] "
                "half");
            if (msg.getbitMask().test(byte)) {
                queued.data[idx] = msg.getdataBlk().getByte(byte);
            }
        }
        return queued;
    }

    queued.chunkValid = 0x3;
    queued.data.resize(64, 0);
    for (unsigned byte = 0; byte < 64; ++byte) {
        if (msg.getbitMask().test(byte)) {
            queued.data[byte] = msg.getdataBlk().getByte(byte);
        }
    }
    return queued;
}

int
toBridgeChannelIdx(Channel ch)
{
    switch (ch) {
        case Channel::RSP:
            return BridgeRsp;
        case Channel::DAT:
            return BridgeDat;
        case Channel::SNP:
            return BridgeSnp;
        case Channel::REQ:
            return BridgeReq;
        case Channel::MISC:
            return BridgeMisc;
        case Channel::NUM_CHANNELS:
            break;
    }

    panic("C2CPacketizerBridge: invalid packetizer channel");
}

unsigned
availableSlotBudget(MessageBuffer *buffer, Tick curTk, unsigned limit)
{
    unsigned budget = 0;
    while (budget < limit && buffer->areNSlotsAvailable(budget + 1, curTk)) {
        budget++;
    }

    return budget;
}

} // namespace

C2CPacketizerBridge::BridgeStats::BridgeStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(req_c2c, statistics::units::Count::get(),
               "REQ messages forwarded across C2C link"),
      ADD_STAT(snp_c2c, statistics::units::Count::get(),
               "SNP messages forwarded across C2C link"),
      ADD_STAT(rsp_c2c, statistics::units::Count::get(),
               "RSP messages forwarded across C2C link"),
      ADD_STAT(dat_c2c, statistics::units::Count::get(),
               "DAT messages forwarded across C2C link"),
      ADD_STAT(containers_sent, statistics::units::Count::get(),
               "Total containers dispatched across C2C link")
{}

C2CPacketizerBridge::C2CPacketizerBridge(const Params &p)
    : ClockedObject(p),
      Consumer(this),
      containerLatency(Cycles(p.container_latency)),
      txqSize(p.txq_size),
      bufferedBytes(0),
      rubySystem(p.ruby_system),
      txCreditMgr(nullptr),
      rxCreditMgr(nullptr),
      bridgeStats(this)
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

C2CPacketizerBridge::BufferedMsg
C2CPacketizerBridge::buildBufferedMsg(const MsgPtr &msg, int ch) const
{
    QueuedMsg queued;

    switch (ch) {
        case CH_REQ:
        case CH_SNP: {
            auto *req = dynamic_cast<const CHIRequestMsg *>(msg.get());
            panic_if(!req,
                     "C2CPacketizerBridge: expected CHIRequestMsg on ch=%d",
                     ch);
            queued.channel = toPacketizerChannel(ch);
            queued.type = requestMsgType(*req, ch);
            break;
        }
        case CH_RSP: {
            auto *rsp = dynamic_cast<const CHIResponseMsg *>(msg.get());
            panic_if(!rsp,
                     "C2CPacketizerBridge: expected CHIResponseMsg on ch=%d",
                     ch);
            queued.channel = Channel::RSP;
            queued.type = responseMsgType(*rsp);
            break;
        }
        case CH_DAT: {
            auto *dat = dynamic_cast<const CHIDataMsg *>(msg.get());
            panic_if(!dat, "C2CPacketizerBridge: expected CHIDataMsg on ch=%d",
                     ch);
            queued = buildQueuedMsgPayload(*dat, dataMsgType(*dat));
            break;
        }
        case CH_MISC: {
            auto *misc = dynamic_cast<const CHI_C2C_MiscMsg *>(msg.get());
            panic_if(!misc,
                     "C2CPacketizerBridge: expected CHI_C2C_MiscMsg on ch=%d",
                     ch);
            queued.channel = Channel::MISC;
            queued.type = miscMsgType(*misc);
            break;
        }
        default:
            panic("C2CPacketizerBridge: invalid channel %d", ch);
    }

    return BufferedMsg{msg, queued, wireBytesForMsgType(queued.type)};
}

void
C2CPacketizerBridge::applyReadyCredits(Tick curTk)
{
    while (!pendingCredits.empty() &&
           pendingCredits.front().readyTick <= curTk) {
        const auto pending = pendingCredits.front();
        pendingCredits.pop_front();
        rxCreditMgr->applyReturnCredits(pending.req, pending.rsp, pending.dat,
                                        pending.snp);
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

void
C2CPacketizerBridge::wakeup()
{
    Tick curTk = curTick();

    applyReadyCredits(curTk);

    for (int i = 0; i < NUM_CHANNELS; i++) {
        while (inBuf[i]->isReady(curTk)) {
            MsgPtr msg = inBuf[i]->peekMsgPtr();
            BufferedMsg buffered = buildBufferedMsg(msg, i);

            if (txqSize > 0) {
                const unsigned txqBytes = txqSize * GRANULE_SIZE;
                panic_if(buffered.wireBytes > txqBytes,
                         "%s: txq_size=%u cannot hold single %u-byte "
                         "message on channel %d",
                         name(), txqSize, buffered.wireBytes, i);
                if (bufferedBytes + buffered.wireBytes > txqBytes) {
                    scheduleEvent(Cycles(1));
                    break;
                }
            }

            bufferedBytes += buffered.wireBytes;
            msgQueues[i].push_back(std::move(buffered));
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

    if (!pendingCredits.empty()) {
        scheduleEventAbsolute(pendingCredits.front().readyTick);
    }
}

void
C2CPacketizerBridge::packAndDeliver()
{
    Tick curTk = curTick();
    Tick delta = cyclesToTicks(containerLatency);
    ContainerPacker packer;

    for (int i = 0; i < NUM_CHANNELS; i++) {
        unsigned slotBudget =
            availableSlotBudget(outBuf[i], curTk, msgQueues[i].size());
        for (unsigned j = 0; j < slotBudget; j++) {
            const auto &buffered = msgQueues[i][j];
            packer.addMessage(buffered.queued);
        }
    }

    uint8_t crReq = 0, crRsp = 0, crDat = 0, crSnp = 0;
    txCreditMgr->drainReturnPending(crReq, crRsp, crDat, crSnp);
    packer.setCreditReturns(crReq, crRsp, crDat, crSnp);

    auto packed = packer.packContainerDetailed();
    if (!packed.has_value()) {
        return;
    }

    for (const auto &queued : packed->packedMessages) {
        int ch = toBridgeChannelIdx(queued.channel);

        panic_if(msgQueues[ch].empty(),
                 "%s: packer selected empty bridge queue for ch=%d", name(),
                 ch);

        const auto &buffered = msgQueues[ch].front();
        panic_if(buffered.queued.type != queued.type,
                 "%s: queued MsgType mismatch on ch=%d (expected %d, got %d)",
                 name(), ch, static_cast<int>(buffered.queued.type),
                 static_cast<int>(queued.type));

        MsgPtr msg = buffered.msg;
        unsigned wireBytes = buffered.wireBytes;
        msgQueues[ch].pop_front();
        panic_if(bufferedBytes < wireBytes,
                 "%s: bufferedBytes underflow (%u < %u)", name(),
                 bufferedBytes, wireBytes);
        bufferedBytes -= wireBytes;

        outBuf[ch]->enqueue(msg, curTk, delta, rubySystem->getRandomization(),
                            rubySystem->getWarmupEnabled());

        switch (ch) {
            case CH_REQ:
                bridgeStats.req_c2c++;
                break;
            case CH_SNP:
                bridgeStats.snp_c2c++;
                break;
            case CH_RSP:
                bridgeStats.rsp_c2c++;
                break;
            case CH_DAT:
                bridgeStats.dat_c2c++;
                break;
            default:
                break;
        }
    }

    bridgeStats.containers_sent++;
    if (crReq || crRsp || crDat || crSnp) {
        pendingCredits.push_back({curTk + delta, crReq, crRsp, crDat, crSnp});
        scheduleEventAbsolute(curTk + delta);
    }

    DPRINTF(RubyCHIC2CPacketizer,
            "Sent container msgStart=0x%x used=%u msgs=%zu credits"
            "[req=%u rsp=%u dat=%u snp=%u]\n",
            packed->container.getMsgStartRaw(),
            packed->container.granulesUsed(), packed->packedMessages.size(),
            crReq, crRsp, crDat, crSnp);
}

} // namespace ruby
} // namespace gem5
