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

#include "mem/ruby/protocol/chi/c2c/C2CContainer.hh"

#include <cassert>
#include <cstring>

namespace gem5
{

namespace ruby
{

namespace chi_c2c
{

unsigned
granulesForMsgType(MsgType type)
{
    switch (type) {
        case MsgType::Resp:
        case MsgType::Snoop:
        case MsgType::MiscU:
        case MsgType::MiscC:
            return 1;
        case MsgType::ReqS:
        case MsgType::ReqL:
        case MsgType::Resp2:
            return 2;
        case MsgType::DataS:
        case MsgType::WrReqS:
            return 3;
        case MsgType::DataL:
        case MsgType::WrReqL:
            return 5;
        default:
            assert(false && "Unknown MsgType");
            return 0;
    }
}

C2CContainer::C2CContainer()
{
    std::memset(&hdr_, 0, sizeof(hdr_));
    payload_.fill(0);
}

bool
C2CContainer::msgStart(unsigned granule) const
{
    assert(granule >= 1 && granule <= NUM_MSG_GRANULES);
    return (hdr_.msgStart >> (granule - 1)) & 1;
}

void
C2CContainer::setMsgStart(unsigned granule, bool val)
{
    assert(granule >= 1 && granule <= NUM_MSG_GRANULES);
    if (val) {
        hdr_.msgStart |= (1u << (granule - 1));
    } else {
        hdr_.msgStart &= ~(1u << (granule - 1));
    }
}

unsigned
C2CContainer::granulesUsed() const
{
    unsigned count = 0;
    unsigned g = 1;
    while (g <= NUM_MSG_GRANULES) {
        if (msgStart(g)) {
            unsigned next = g + 1;
            while (next <= NUM_MSG_GRANULES && !msgStart(next)) {
                next++;
            }
            count += (next - g);
            g = next;
        } else {
            g++;
        }
    }
    return count;
}

float
C2CContainer::fullnessRatio() const
{
    return static_cast<float>(granulesUsed()) / NUM_MSG_GRANULES;
}

uint8_t *
C2CContainer::granuleData(unsigned granule)
{
    assert(granule >= 1 && granule <= NUM_MSG_GRANULES);
    return &payload_[(granule - 1) * GRANULE_SIZE];
}

const uint8_t *
C2CContainer::granuleData(unsigned granule) const
{
    assert(granule >= 1 && granule <= NUM_MSG_GRANULES);
    return &payload_[(granule - 1) * GRANULE_SIZE];
}

unsigned
C2CContainer::granuleSize(unsigned granule) const
{
    assert(granule >= 1 && granule <= NUM_MSG_GRANULES);
    // Granule 12 is only 16 bytes (256 - 240 = 16)
    if (granule == NUM_MSG_GRANULES) {
        return CONTAINER_SIZE - PROTHDR_SIZE -
               (NUM_MSG_GRANULES - 1) * GRANULE_SIZE;
    }
    return GRANULE_SIZE;
}

void
C2CContainer::serialize(uint8_t *buf) const
{
    std::memcpy(buf, &hdr_, PROTHDR_SIZE);
    std::memcpy(buf + PROTHDR_SIZE, payload_.data(), payload_.size());
}

void
C2CContainer::deserialize(const uint8_t *buf)
{
    std::memcpy(&hdr_, buf, PROTHDR_SIZE);
    std::memcpy(payload_.data(), buf + PROTHDR_SIZE, payload_.size());
}

} // namespace chi_c2c
} // namespace ruby
} // namespace gem5
