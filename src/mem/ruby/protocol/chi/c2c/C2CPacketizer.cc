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

namespace
{

struct GranuleState
{
    unsigned responseSlots = 0;
    bool fullMessage = false;

    bool
    used() const
    {
        return responseSlots > 0 || fullMessage;
    }
};

struct GroupState
{
    std::array<GranuleState, GRANULES_PER_GROUP> granules = {};
    unsigned responseSlotsTotal = 0;
    bool hasMiscU = false;
};

struct PlacementSite
{
    unsigned startGranule;
    unsigned intraGranuleOffset;
    unsigned sortKey;
    std::array<GroupState, NUM_GRANULE_GROUPS> nextState;
};

unsigned
groupPrefixLen(const GroupState &group)
{
    for (unsigned idx = 0; idx < GRANULES_PER_GROUP; ++idx) {
        if (!group.granules[idx].used()) {
            return idx;
        }
    }

    return GRANULES_PER_GROUP;
}

unsigned
absoluteWireOffset(unsigned granule, unsigned intraGranuleOffset = 0)
{
    return (granule - 1) * GRANULE_SIZE + intraGranuleOffset;
}

uint8_t
normalizedChunkValid(const QueuedMsg &msg)
{
    if (!isDataMsgType(msg.type)) {
        return 0;
    }

    if (isShortDataMsgType(msg.type)) {
        if (msg.chunkValid != 0) {
            return msg.chunkValid;
        }
        return msg.shortDataUpper ? 0x2 : 0x1;
    }

    return msg.chunkValid != 0 ? msg.chunkValid : 0x3;
}

std::vector<uint8_t>
encodeMessage(const QueuedMsg &msg)
{
    const unsigned totalBytes = wireBytesForMsgType(msg.type);
    std::vector<uint8_t> payload(totalBytes, 0);
    payload[0] = wireCodeForMsgType(msg.type);

    unsigned payloadOffset = 1;
    if (isDataMsgType(msg.type)) {
        payload[1] = normalizedChunkValid(msg) & 0x3;
        payloadOffset = 2;
    }

    if (payloadOffset < totalBytes && !msg.data.empty()) {
        const unsigned toCopy =
            std::min(totalBytes - payloadOffset,
                     static_cast<unsigned>(msg.data.size()));
        std::memcpy(payload.data() + payloadOffset, msg.data.data(), toCopy);
    }

    return payload;
}

PackedMsg
decodeFullMessage(const C2CContainer &container, unsigned startGranule,
                  MsgType type)
{
    PackedMsg message;
    message.startGranule = startGranule;
    message.intraGranuleOffset = 0;
    message.numGranules = granulesForMsgType(type);
    message.type = type;

    const unsigned totalBytes = wireBytesForMsgType(type);
    message.data.reserve(totalBytes);
    for (unsigned granule = startGranule;
         granule < startGranule + message.numGranules; ++granule) {
        const uint8_t *ptr = container.granuleData(granule);
        const unsigned bytesLeft = totalBytes - message.data.size();
        const unsigned toCopy = std::min(bytesLeft, GRANULE_SIZE);
        message.data.insert(message.data.end(), ptr, ptr + toCopy);
    }

    if (isDataMsgType(type)) {
        message.chunkValid = message.data[1] & 0x3;
        message.dataField.assign(64, 0);

        const uint8_t *wireData = message.data.data() + 2;
        if (isShortDataMsgType(type)) {
            const unsigned halfOffset = message.chunkValid == 0x2 ? 32 : 0;
            const unsigned toCopy =
                std::min(32u, static_cast<unsigned>(message.data.size() - 2));
            std::copy(wireData, wireData + toCopy,
                      message.dataField.begin() + halfOffset);
        } else {
            const unsigned toCopy =
                std::min(64u, static_cast<unsigned>(message.data.size() - 2));
            std::copy(wireData, wireData + toCopy, message.dataField.begin());
        }
    }

    return message;
}

PackedMsg
decodeResponseMessage(const C2CContainer &container, unsigned granule,
                      unsigned intraGranuleOffset, MsgType type)
{
    PackedMsg message;
    message.startGranule = granule;
    message.intraGranuleOffset = intraGranuleOffset;
    message.numGranules = 1;
    message.type = type;

    const unsigned totalBytes = wireBytesForMsgType(type);
    const uint8_t *ptr = container.granuleData(granule) + intraGranuleOffset;
    message.data.insert(message.data.end(), ptr, ptr + totalBytes);
    return message;
}

std::optional<PlacementSite>
findResponsePlacement(const std::array<GroupState, NUM_GRANULE_GROUPS> &state,
                      const QueuedMsg &msg, unsigned minSortKey)
{
    const unsigned slotsNeeded = responseSlotUnitsForMsgType(msg.type);

    for (unsigned group = 0; group < NUM_GRANULE_GROUPS; ++group) {
        if (state[group].responseSlotsTotal + slotsNeeded >
            RESP_SLOTS_PER_GROUP) {
            continue;
        }

        const unsigned prefixLen = groupPrefixLen(state[group]);
        for (unsigned index = 0; index < GRANULES_PER_GROUP; ++index) {
            const auto &granule = state[group].granules[index];
            if (granule.fullMessage) {
                continue;
            }

            if (!granule.used() && index != prefixLen) {
                continue;
            }

            if (msg.type == MsgType::Resp2) {
                if (granule.responseSlots != 0) {
                    continue;
                }
            } else if (granule.responseSlots >= RESP_SLOTS_PER_GRANULE) {
                continue;
            }

            auto updated = state;
            unsigned intraOffset =
                updated[group].granules[index].responseSlots *
                RESPONSE_SLOT_SIZE;
            updated[group].granules[index].responseSlots += slotsNeeded;
            updated[group].responseSlotsTotal += slotsNeeded;

            const unsigned granuleNum = group * GRANULES_PER_GROUP + index + 1;
            const unsigned sortKey =
                absoluteWireOffset(granuleNum, intraOffset);
            if (sortKey < minSortKey) {
                continue;
            }
            return PlacementSite{
                granuleNum,
                intraOffset,
                sortKey,
                updated,
            };
        }
    }

    return std::nullopt;
}

std::optional<PlacementSite>
findFullMessagePlacement(
    const std::array<GroupState, NUM_GRANULE_GROUPS> &state,
    const QueuedMsg &msg, unsigned minSortKey)
{
    const unsigned needed = granulesForMsgType(msg.type);

    for (unsigned start = 1; start + needed - 1 <= NUM_MSG_GRANULES; ++start) {
        auto updated = state;
        bool fits = true;

        for (unsigned offset = 0; offset < needed; ++offset) {
            const unsigned granule = start + offset;
            const unsigned group = granuleGroup(granule);
            const unsigned index = granuleIndexInGroup(granule);
            auto &slot = updated[group].granules[index];

            if (slot.fullMessage || slot.responseSlots > 0) {
                fits = false;
                break;
            }

            const unsigned prefixLen = groupPrefixLen(updated[group]);
            if (index != prefixLen) {
                fits = false;
                break;
            }

            slot.fullMessage = true;
            if (offset == 0 && msg.type == MsgType::MiscU) {
                if (updated[group].hasMiscU) {
                    fits = false;
                    break;
                }
                updated[group].hasMiscU = true;
            }
        }

        if (fits) {
            const unsigned sortKey = absoluteWireOffset(start);
            if (sortKey < minSortKey) {
                continue;
            }
            return PlacementSite{start, 0, sortKey, updated};
        }
    }

    return std::nullopt;
}

std::optional<PlacementSite>
findPlacement(const std::array<GroupState, NUM_GRANULE_GROUPS> &state,
              const QueuedMsg &msg, unsigned minSortKey)
{
    if (isResponseMsgType(msg.type)) {
        return findResponsePlacement(state, msg, minSortKey);
    }

    return findFullMessagePlacement(state, msg, minSortKey);
}

void
writeMessage(C2CContainer &container, const PlacementSite &site,
             const QueuedMsg &msg)
{
    const auto encoded = encodeMessage(msg);

    if (isResponseMsgType(msg.type)) {
        uint8_t *ptr =
            container.granuleData(site.startGranule) + site.intraGranuleOffset;
        std::memcpy(ptr, encoded.data(), encoded.size());
        if (site.intraGranuleOffset == 0) {
            container.setMsgStart(site.startGranule, true);
        }
        container.setGranuleOccupied(site.startGranule, true);
        return;
    }

    container.setMsgStart(site.startGranule, true);
    unsigned copied = 0;
    for (unsigned granule = site.startGranule;
         granule < site.startGranule + granulesForMsgType(msg.type);
         ++granule) {
        const unsigned bytesLeft = encoded.size() - copied;
        const unsigned toCopy = std::min(bytesLeft, GRANULE_SIZE);
        std::memcpy(container.granuleData(granule), encoded.data() + copied,
                    toCopy);
        container.setGranuleOccupied(granule, true);
        copied += toCopy;
    }
}

bool
validateDataChunkValid(MsgType type, uint8_t chunkValid)
{
    if (!isDataMsgType(type)) {
        return true;
    }

    if (isShortDataMsgType(type)) {
        return chunkValid == 0x1 || chunkValid == 0x2;
    }

    return chunkValid == 0x3;
}

} // namespace

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
    auto detailed = packContainerDetailed();
    if (!detailed.has_value()) {
        return std::nullopt;
    }

    return detailed->container;
}

std::optional<PackedContainer>
ContainerPacker::packContainerDetailed()
{
    if (!hasMessages() && pendingReqCredit_ == 0 && pendingRspCredit_ == 0 &&
        pendingDatCredit_ == 0 && pendingSnpCredit_ == 0) {
        return std::nullopt;
    }

    PackedContainer result;
    C2CContainer &container = result.container;
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

    std::array<GroupState, NUM_GRANULE_GROUPS> groupState = {};
    struct Placement
    {
        unsigned sortKey;
        QueuedMsg msg;
    };
    std::vector<Placement> placements;
    unsigned minSortKey = 0;

    for (Channel ch : priority_) {
        auto &queue = queues_[static_cast<unsigned>(ch)];
        while (!queue.empty()) {
            auto site = findPlacement(groupState, queue.front(), minSortKey);
            if (!site) {
                break;
            }

            QueuedMsg msg = std::move(queue.front());
            queue.pop_front();
            writeMessage(container, *site, msg);
            groupState = site->nextState;
            minSortKey = site->sortKey;
            placements.push_back({site->sortKey, std::move(msg)});
        }
    }

    std::sort(placements.begin(), placements.end(),
              [](const Placement &lhs, const Placement &rhs) {
                  return lhs.sortKey < rhs.sortKey;
              });

    for (auto &placement : placements) {
        result.packedMessages.push_back(std::move(placement.msg));
    }

    return result;
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
    if (raw & ~((1u << NUM_MSG_GRANULES) - 1)) {
        return {false, ContainerError::MSGSTART_OUT_OF_RANGE};
    }

    std::array<bool, NUM_MSG_GRANULES> covered = {};
    std::array<unsigned, NUM_GRANULE_GROUPS> responseSlots = {};
    std::array<unsigned, NUM_GRANULE_GROUPS> miscUCount = {};

    for (unsigned granule = 1; granule <= NUM_MSG_GRANULES; ++granule) {
        if (container_.msgStart(granule)) {
            const auto type =
                decodeMsgType(container_.granuleData(granule)[0]);
            if (!type) {
                return {false, ContainerError::INVALID_MSGTYPE};
            }

            if (isResponseMsgType(*type)) {
                covered[granule - 1] = true;

                unsigned slots = responseSlotUnitsForMsgType(*type);
                const auto second = decodeMsgType(
                    container_.granuleData(granule)[RESPONSE_SLOT_SIZE]);
                if (*type == MsgType::Resp) {
                    if (second) {
                        if (*second != MsgType::Resp) {
                            return {false,
                                    ContainerError::INVALID_RESPONSE_PACKING};
                        }
                        slots += 1;
                    }
                } else if (second) {
                    return {false, ContainerError::INVALID_RESPONSE_PACKING};
                }

                const unsigned group = granuleGroup(granule);
                responseSlots[group] += slots;
                if (responseSlots[group] > RESP_SLOTS_PER_GROUP) {
                    return {false,
                            ContainerError::TOO_MANY_RESPONSES_IN_GROUP};
                }
                continue;
            }

            if (isDataMsgType(*type) &&
                !validateDataChunkValid(
                    *type, container_.granuleData(granule)[1] & 0x3)) {
                return {false, ContainerError::INVALID_CHUNK_VALID};
            }

            if (*type == MsgType::MiscU) {
                const unsigned group = granuleGroup(granule);
                miscUCount[group]++;
                if (miscUCount[group] > 1) {
                    return {false, ContainerError::MULTIPLE_MISCU_IN_GROUP};
                }
            }

            const unsigned needed = granulesForMsgType(*type);
            if (granule + needed - 1 > NUM_MSG_GRANULES) {
                return {false, ContainerError::MESSAGE_OVERRUN};
            }

            for (unsigned body = granule + 1; body < granule + needed;
                 ++body) {
                if (container_.msgStart(body)) {
                    return {false,
                            ContainerError::UNEXPECTED_MSGSTART_IN_BODY};
                }
            }

            for (unsigned g = granule; g < granule + needed; ++g) {
                covered[g - 1] = true;
            }
        } else if (!covered[granule - 1] &&
                   granuleHasData(container_, granule)) {
            return {false, ContainerError::NONZERO_GRANULE_WITHOUT_START};
        }
    }

    for (unsigned group = 0; group < NUM_GRANULE_GROUPS; ++group) {
        bool sawHole = false;
        for (unsigned slot = 0; slot < GRANULES_PER_GROUP; ++slot) {
            const unsigned granule = group * GRANULES_PER_GROUP + slot + 1;
            const bool used =
                covered[granule - 1] || granuleHasData(container_, granule);
            if (!used) {
                sawHole = true;
                continue;
            }

            if (sawHole) {
                return {false, ContainerError::INVALID_GROUP_OCCUPANCY};
            }
        }
    }

    return {true, ContainerError::NONE};
}

std::vector<PackedMsg>
ContainerUnpacker::extractAll() const
{
    std::vector<PackedMsg> messages;
    unsigned granule = 1;

    while (granule <= NUM_MSG_GRANULES) {
        if (!container_.msgStart(granule)) {
            granule++;
            continue;
        }

        const auto type = decodeMsgType(container_.granuleData(granule)[0]);
        if (!type) {
            granule++;
            continue;
        }

        if (isResponseMsgType(*type)) {
            messages.push_back(
                decodeResponseMessage(container_, granule, 0, *type));

            if (*type == MsgType::Resp) {
                const auto second = decodeMsgType(
                    container_.granuleData(granule)[RESPONSE_SLOT_SIZE]);
                if (second && *second == MsgType::Resp) {
                    messages.push_back(decodeResponseMessage(
                        container_, granule, RESPONSE_SLOT_SIZE, *second));
                }
            }

            granule++;
            continue;
        }

        messages.push_back(decodeFullMessage(container_, granule, *type));
        granule += granulesForMsgType(*type);
    }

    return messages;
}

} // namespace chi_c2c
} // namespace ruby
} // namespace gem5
