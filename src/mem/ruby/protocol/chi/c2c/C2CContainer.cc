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

#include <algorithm>
#include <cassert>
#include <cstring>

namespace gem5
{

namespace ruby
{

namespace chi_c2c
{

namespace
{

constexpr std::array<unsigned, NUM_MSG_GRANULES> GranuleWireOffsets = {
    2, 22, 42, 66, 86, 106, 130, 150, 170, 194, 214, 234,
};

constexpr std::array<unsigned, PROTHDR_WIRE_SIZE> ProtHdrWireOffsets = {
    62, 63, 64, 65, 128, 129, 190, 191, 192, 193,
};

} // namespace

unsigned
granulesForMsgType(MsgType type)
{
    switch (type) {
        case MsgType::Resp:
        case MsgType::Resp2:
        case MsgType::Snoop:
        case MsgType::MiscU:
        case MsgType::MiscC:
            return 1;
        case MsgType::ReqS:
        case MsgType::ReqL:
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

unsigned
wireBytesForMsgType(MsgType type)
{
    if (isResponseMsgType(type)) {
        return responseSlotUnitsForMsgType(type) * RESPONSE_SLOT_SIZE;
    }

    return granulesForMsgType(type) * GRANULE_SIZE;
}

unsigned
responseSlotUnitsForMsgType(MsgType type)
{
    switch (type) {
        case MsgType::Resp:
            return 1;
        case MsgType::Resp2:
            return 2;
        default:
            return 0;
    }
}

uint8_t
wireCodeForMsgType(MsgType type)
{
    switch (type) {
        case MsgType::Resp:
            return 0x11;
        case MsgType::Resp2:
            return 0x12;
        case MsgType::Snoop:
            return 0x21;
        case MsgType::MiscU:
            return 0x31;
        case MsgType::MiscC:
            return 0x32;
        case MsgType::ReqS:
            return 0x41;
        case MsgType::ReqL:
            return 0x42;
        case MsgType::DataS:
            return 0x51;
        case MsgType::WrReqS:
            return 0x52;
        case MsgType::DataL:
            return 0x61;
        case MsgType::WrReqL:
            return 0x62;
        default:
            assert(false && "Unknown MsgType");
            return 0;
    }
}

std::optional<MsgType>
decodeMsgType(uint8_t raw)
{
    switch (raw) {
        case 0x11:
            return MsgType::Resp;
        case 0x12:
            return MsgType::Resp2;
        case 0x21:
            return MsgType::Snoop;
        case 0x31:
            return MsgType::MiscU;
        case 0x32:
            return MsgType::MiscC;
        case 0x41:
            return MsgType::ReqS;
        case 0x42:
            return MsgType::ReqL;
        case 0x51:
            return MsgType::DataS;
        case 0x52:
            return MsgType::WrReqS;
        case 0x61:
            return MsgType::DataL;
        case 0x62:
            return MsgType::WrReqL;
        default:
            return std::nullopt;
    }
}

bool
isResponseMsgType(MsgType type)
{
    return type == MsgType::Resp || type == MsgType::Resp2;
}

bool
isDataMsgType(MsgType type)
{
    switch (type) {
        case MsgType::DataS:
        case MsgType::WrReqS:
        case MsgType::DataL:
        case MsgType::WrReqL:
            return true;
        default:
            return false;
    }
}

bool
isShortDataMsgType(MsgType type)
{
    return type == MsgType::DataS || type == MsgType::WrReqS;
}

unsigned
granuleGroup(unsigned granule)
{
    assert(granule >= 1 && granule <= NUM_MSG_GRANULES);
    return (granule - 1) / GRANULES_PER_GROUP;
}

unsigned
granuleIndexInGroup(unsigned granule)
{
    assert(granule >= 1 && granule <= NUM_MSG_GRANULES);
    return (granule - 1) % GRANULES_PER_GROUP;
}

bool
granuleHasData(const C2CContainer &container, unsigned granule)
{
    const uint8_t *ptr = container.granuleData(granule);
    return std::any_of(ptr, ptr + GRANULE_SIZE,
                       [](uint8_t byte) { return byte != 0; });
}

C2CContainer::C2CContainer()
{
    std::memset(&hdr_, 0, sizeof(hdr_));
    granules_.fill(0);
    occupied_.fill(false);
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
        occupied_[granule - 1] = true;
    } else {
        hdr_.msgStart &= ~(1u << (granule - 1));
    }
}

unsigned
C2CContainer::granulesUsed() const
{
    return std::count(occupied_.begin(), occupied_.end(), true);
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
    return &granules_[(granule - 1) * GRANULE_SIZE];
}

const uint8_t *
C2CContainer::granuleData(unsigned granule) const
{
    assert(granule >= 1 && granule <= NUM_MSG_GRANULES);
    return &granules_[(granule - 1) * GRANULE_SIZE];
}

bool
C2CContainer::granuleOccupied(unsigned granule) const
{
    assert(granule >= 1 && granule <= NUM_MSG_GRANULES);
    return occupied_[granule - 1];
}

void
C2CContainer::setGranuleOccupied(unsigned granule, bool occupied)
{
    assert(granule >= 1 && granule <= NUM_MSG_GRANULES);
    occupied_[granule - 1] = occupied;
}

unsigned
C2CContainer::granuleSize(unsigned granule) const
{
    assert(granule >= 1 && granule <= NUM_MSG_GRANULES);
    return GRANULE_SIZE;
}

void
C2CContainer::serialize(uint8_t *buf) const
{
    std::memset(buf, 0, CONTAINER_SIZE);

    std::array<uint8_t, PROTHDR_WIRE_SIZE> hdrBytes = {};
    hdrBytes[0] = (hdr_.reqCredit & 0x0f) | ((hdr_.rspCredit & 0x0f) << 4);
    hdrBytes[1] = (hdr_.datCredit & 0x0f) | ((hdr_.snpCredit & 0x0f) << 4);
    hdrBytes[2] = hdr_.msgStart & 0xff;
    hdrBytes[3] = (hdr_.msgStart >> 8) & 0x0f;

    for (unsigned i = 0; i < PROTHDR_WIRE_SIZE; ++i) {
        buf[ProtHdrWireOffsets[i]] = hdrBytes[i];
    }

    for (unsigned granule = 1; granule <= NUM_MSG_GRANULES; ++granule) {
        std::memcpy(buf + GranuleWireOffsets[granule - 1],
                    granuleData(granule), GRANULE_SIZE);
    }
}

void
C2CContainer::deserialize(const uint8_t *buf)
{
    std::array<uint8_t, PROTHDR_WIRE_SIZE> hdrBytes = {};
    for (unsigned i = 0; i < PROTHDR_WIRE_SIZE; ++i) {
        hdrBytes[i] = buf[ProtHdrWireOffsets[i]];
    }

    hdr_.reqCredit = hdrBytes[0] & 0x0f;
    hdr_.rspCredit = (hdrBytes[0] >> 4) & 0x0f;
    hdr_.datCredit = hdrBytes[1] & 0x0f;
    hdr_.snpCredit = (hdrBytes[1] >> 4) & 0x0f;
    hdr_.msgStart = static_cast<uint16_t>(hdrBytes[2]) |
                    (static_cast<uint16_t>(hdrBytes[3] & 0x0f) << 8);
    hdr_.containerValid = 0;

    for (unsigned granule = 1; granule <= NUM_MSG_GRANULES; ++granule) {
        std::memcpy(granuleData(granule),
                    buf + GranuleWireOffsets[granule - 1], GRANULE_SIZE);
    }

    occupied_.fill(false);
    for (unsigned granule = 1; granule <= NUM_MSG_GRANULES; ++granule) {
        if (!msgStart(granule)) {
            continue;
        }

        const auto type = decodeMsgType(granuleData(granule)[0]);
        if (!type) {
            occupied_[granule - 1] = true;
            continue;
        }

        occupied_[granule - 1] = true;
        if (isResponseMsgType(*type)) {
            continue;
        }

        const unsigned needed = granulesForMsgType(*type);
        if (granule + needed - 1 > NUM_MSG_GRANULES) {
            continue;
        }

        for (unsigned g = granule; g < granule + needed; ++g) {
            occupied_[g - 1] = true;
        }
    }

    for (unsigned granule = 1; granule <= NUM_MSG_GRANULES; ++granule) {
        if (!occupied_[granule - 1] && granuleHasData(*this, granule)) {
            occupied_[granule - 1] = true;
        }
    }

    hdr_.containerValid = hdr_.msgStart || hdr_.reqCredit || hdr_.rspCredit ||
                          hdr_.datCredit || hdr_.snpCredit;
}

} // namespace chi_c2c
} // namespace ruby
} // namespace gem5
