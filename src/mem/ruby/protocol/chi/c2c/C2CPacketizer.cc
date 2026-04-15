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

#include "mem/ruby/protocol/chi/c2c/C2CPacketizer.hh"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace gem5
{

namespace ruby
{

namespace chi_c2c
{

// --------------------------------------------------------------------------
// ContainerPacker
// --------------------------------------------------------------------------

ContainerPacker::ContainerPacker()
    : priority_({Channel::RSP, Channel::DAT, Channel::SNP, Channel::REQ,
                 Channel::MISC}),
      pendingReqCredit_(0),
      pendingRspCredit_(0),
      pendingDatCredit_(0),
      pendingSnpCredit_(0)
{}

void
ContainerPacker::addMessage(const QueuedMsg &msg)
{
    unsigned idx = static_cast<unsigned>(msg.channel);
    assert(idx < NUM_CH);
    queues_[idx].push_back(msg);
}

bool
ContainerPacker::hasMessages() const
{
    for (const auto &q : queues_) {
        if (!q.empty()) {
            return true;
        }
    }
    return false;
}

void
ContainerPacker::setCreditReturns(uint8_t req, uint8_t rsp, uint8_t dat,
                                  uint8_t snp)
{
    pendingReqCredit_ = std::min(req, uint8_t(15));
    pendingRspCredit_ = std::min(rsp, uint8_t(15));
    pendingDatCredit_ = std::min(dat, uint8_t(15));
    pendingSnpCredit_ = std::min(snp, uint8_t(15));
}

void
ContainerPacker::setPriority(const std::vector<Channel> &order)
{
    assert(order.size() == NUM_CH);
    priority_ = order;
}

std::optional<C2CContainer>
ContainerPacker::packContainer()
{
    if (!hasMessages() && pendingReqCredit_ == 0 && pendingRspCredit_ == 0 &&
        pendingDatCredit_ == 0 && pendingSnpCredit_ == 0) {
        return std::nullopt;
    }

    C2CContainer container;
    ProtHdr &hdr = container.protocolHeader();
    hdr.containerValid = 1;
    hdr.reqCredit = pendingReqCredit_;
    hdr.rspCredit = pendingRspCredit_;
    hdr.datCredit = pendingDatCredit_;
    hdr.snpCredit = pendingSnpCredit_;

    pendingReqCredit_ = 0;
    pendingRspCredit_ = 0;
    pendingDatCredit_ = 0;
    pendingSnpCredit_ = 0;

    unsigned nextGranule = 1;
    unsigned remaining = NUM_MSG_GRANULES;

    // Pack messages in priority order
    for (Channel ch : priority_) {
        auto &queue = queues_[static_cast<unsigned>(ch)];
        while (!queue.empty() && remaining > 0) {
            const QueuedMsg &msg = queue.front();
            unsigned needed = granulesForMsgType(msg.type);
            if (needed > remaining) {
                break; // Can't fit; try next channel
            }

            // Mark the start granule
            container.setMsgStart(nextGranule, true);

            // Copy message data into granule slots
            unsigned bytesCopied = 0;
            for (unsigned g = 0; g < needed; g++) {
                unsigned gran = nextGranule + g;
                unsigned gSize = container.granuleSize(gran);
                unsigned toCopy =
                    std::min(gSize, static_cast<unsigned>(msg.data.size()) -
                                        bytesCopied);
                if (toCopy > 0) {
                    std::memcpy(container.granuleData(gran),
                                msg.data.data() + bytesCopied, toCopy);
                }
                bytesCopied += gSize;
            }

            nextGranule += needed;
            remaining -= needed;
            queue.pop_front();
        }
    }

    return container;
}

// --------------------------------------------------------------------------
// ContainerUnpacker
// --------------------------------------------------------------------------

ContainerUnpacker::ContainerUnpacker(const C2CContainer &container)
    : container_(container)
{}

ValidationResult
ContainerUnpacker::validate() const
{
    uint16_t raw = container_.getMsgStartRaw();
    // Check bits are within valid range (12 bits)
    if (raw & ~((1u << NUM_MSG_GRANULES) - 1)) {
        return {false, ContainerError::MSGSTART_OUT_OF_RANGE};
    }

    return {true, ContainerError::NONE};
}

std::vector<PackedMsg>
ContainerUnpacker::extractAll() const
{
    std::vector<PackedMsg> messages;
    unsigned g = 1;

    while (g <= NUM_MSG_GRANULES) {
        if (!container_.msgStart(g)) {
            g++;
            continue;
        }

        // Find the span: from g until the next start bit or end
        unsigned start = g;
        unsigned end = g + 1;
        while (end <= NUM_MSG_GRANULES && !container_.msgStart(end)) {
            end++;
        }

        unsigned numGran = end - start;

        // Collect the raw data
        PackedMsg pm;
        pm.startGranule = start;
        pm.numGranules = numGran;
        for (unsigned i = start; i < end; i++) {
            unsigned gSize = container_.granuleSize(i);
            const uint8_t *ptr = container_.granuleData(i);
            pm.data.insert(pm.data.end(), ptr, ptr + gSize);
        }

        messages.push_back(std::move(pm));
        g = end;
    }

    return messages;
}

} // namespace chi_c2c
} // namespace ruby
} // namespace gem5
