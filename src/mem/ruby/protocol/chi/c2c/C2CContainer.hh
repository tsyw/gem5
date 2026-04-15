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
#include <vector>

namespace gem5
{

namespace ruby
{

namespace chi_c2c
{

// Format X container: 256 bytes, UCIe-compatible (IHI0098A)
//
// Layout:
//   Granule 0  [  0: 19] -- Protocol Header (ProtHdr)
//   Granule 1  [ 20: 39] -- Message slot
//   ...
//   Granule 11 [220:239] -- Message slot
//   Granule 12 [240:255] -- Partial (16 bytes)
//
// Up to 12 message granules (20 bytes each, except #12 = 16 bytes).

static constexpr unsigned CONTAINER_SIZE = 256;
static constexpr unsigned GRANULE_SIZE = 20;
static constexpr unsigned NUM_MSG_GRANULES = 12;
static constexpr unsigned PROTHDR_SIZE = GRANULE_SIZE; // Granule 0

// Protocol Header (20 bytes / 160 bits)
struct ProtHdr
{
    uint16_t msgStart;      // Bit vector: granule N starts a message
    uint8_t reqCredit;      // Piggybacked REQ credit returns (0-15)
    uint8_t rspCredit;      // Piggybacked RSP credit returns (0-15)
    uint8_t datCredit;      // Piggybacked DAT credit returns (0-15)
    uint8_t snpCredit;      // Piggybacked SNP credit returns (0-15)
    uint8_t containerValid; // Nonzero if container has valid messages
    uint8_t reserved[13];   // Pad to 20 bytes
};
static_assert(sizeof(ProtHdr) == PROTHDR_SIZE,
              "ProtHdr must be exactly 20 bytes");

// A single message packed into one or more granules.
struct PackedMsg
{
    unsigned startGranule; // 1-based granule index
    unsigned numGranules;  // Number of granules occupied
    std::vector<uint8_t> data;
};

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

unsigned granulesForMsgType(MsgType type);

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
    // Raw payload: 236 bytes (granules 1-12)
    std::array<uint8_t, CONTAINER_SIZE - PROTHDR_SIZE> payload_;
};

} // namespace chi_c2c
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_PROTOCOL_CHI_C2C_C2CCONTAINER_HH__
