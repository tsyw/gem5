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

#ifndef __MEM_RUBY_PROTOCOL_CHI_C2C_C2CCONTAINER_HH__
#define __MEM_RUBY_PROTOCOL_CHI_C2C_C2CCONTAINER_HH__

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace gem5
{

namespace ruby
{

namespace chi_c2c
{

// Format X container: 256 bytes, UCIe-compatible (IHI0098A)
//
// Wire layout used by this model:
//   [LinkHdr0][LinkHdr1][G0][G1][G2][ProtHdr0..3]
//   [G3][G4][G5][LinkHdr2][LinkHdr3][ProtHdr4..5]
//   [G6][G7][G8][ProtHdr6..9][G9][G10][G11][LinkHdr4][LinkHdr5]
//
// LinkHdr bytes are reserved/ignored for timing-model purposes. The model
// keeps 12 logical 20-byte message granules (G0..G11) and serializes them into
// the interleaved 256-byte wire image above.

static constexpr unsigned CONTAINER_SIZE = 256;
static constexpr unsigned GRANULE_SIZE = 20;
static constexpr unsigned NUM_MSG_GRANULES = 12;
static constexpr unsigned GRANULES_PER_GROUP = 3;
static constexpr unsigned NUM_GRANULE_GROUPS =
    NUM_MSG_GRANULES / GRANULES_PER_GROUP;
static constexpr unsigned PROTHDR_WIRE_SIZE = 10;
static constexpr unsigned LINKHDR_WIRE_SIZE = 6;
static constexpr unsigned RESPONSE_SLOT_SIZE = 10;
static constexpr unsigned RESP_SLOTS_PER_GRANULE =
    GRANULE_SIZE / RESPONSE_SLOT_SIZE;
static constexpr unsigned RESP_SLOTS_PER_GROUP = 4;

// Granule counts per message type (IHI0098A Table 4.4)
enum class MsgType : uint8_t
{
    Resp = 0,    // 1 granule
    Snoop = 1,   // 1 granule
    MiscU = 2,   // 1 granule
    MiscC = 3,   // 1 granule
    ReqS = 4,    // 2 granules
    ReqL = 5,    // 2 granules
    Resp2 = 6,   // 2 granules
    DataS = 7,   // 3 granules
    WrReqS = 8,  // 3 granules (WritePush Short)
    DataL = 9,   // 5 granules
    WrReqL = 10, // 5 granules (WritePush Long)
};

class C2CContainer;

unsigned granulesForMsgType(MsgType type);
unsigned wireBytesForMsgType(MsgType type);
unsigned responseSlotUnitsForMsgType(MsgType type);
uint8_t wireCodeForMsgType(MsgType type);
std::optional<MsgType> decodeMsgType(uint8_t raw);
bool isResponseMsgType(MsgType type);
bool isDataMsgType(MsgType type);
bool isShortDataMsgType(MsgType type);
unsigned granuleGroup(unsigned granule);
unsigned granuleIndexInGroup(unsigned granule);
bool granuleHasData(const C2CContainer &container, unsigned granule);

// Logical protocol header fields tracked by the timing model. On the wire
// these are packed into 10 dispersed ProtHdr bytes.
struct ProtHdr
{
    uint16_t msgStart;      // Bit vector: granule N starts a message
    uint8_t reqCredit;      // Piggybacked REQ credit returns (0-15)
    uint8_t rspCredit;      // Piggybacked RSP credit returns (0-15)
    uint8_t datCredit;      // Piggybacked DAT credit returns (0-15)
    uint8_t snpCredit;      // Piggybacked SNP credit returns (0-15)
    uint8_t containerValid; // Nonzero if container has valid messages
};

// A single message packed into one or more granules.
struct PackedMsg
{
    unsigned startGranule;       // 1-based granule index
    unsigned intraGranuleOffset; // 0 or 10 for packed responses
    unsigned numGranules;        // Number of granules occupied
    MsgType type;
    uint8_t chunkValid = 0;
    std::vector<uint8_t> data;
    std::vector<uint8_t> dataField;
};

// A 256-byte Format X container.
class C2CContainer
{
  public:
    C2CContainer();

    ProtHdr &
    protocolHeader()
    {
        return hdr_;
    }
    const ProtHdr &
    protocolHeader() const
    {
        return hdr_;
    }

    bool msgStart(unsigned granule) const;
    void setMsgStart(unsigned granule, bool val);
    uint16_t
    getMsgStartRaw() const
    {
        return hdr_.msgStart;
    }
    void
    setMsgStartRaw(uint16_t raw)
    {
        hdr_.msgStart = raw;
    }

    unsigned granulesUsed() const;
    unsigned
    granulesTotal() const
    {
        return NUM_MSG_GRANULES;
    }
    float fullnessRatio() const;

    // Granule index is 1-based (1..12)
    uint8_t *granuleData(unsigned granule);
    const uint8_t *granuleData(unsigned granule) const;
    bool granuleOccupied(unsigned granule) const;
    void setGranuleOccupied(unsigned granule, bool occupied);
    unsigned granuleSize(unsigned granule) const;

    unsigned
    size() const
    {
        return CONTAINER_SIZE;
    }

    void serialize(uint8_t *buf) const;
    void deserialize(const uint8_t *buf);

  private:
    ProtHdr hdr_;
    std::array<uint8_t, NUM_MSG_GRANULES * GRANULE_SIZE> granules_;
    std::array<bool, NUM_MSG_GRANULES> occupied_;
};

} // namespace chi_c2c
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_PROTOCOL_CHI_C2C_C2CCONTAINER_HH__
