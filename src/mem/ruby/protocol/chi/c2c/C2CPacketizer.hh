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

#ifndef __MEM_RUBY_PROTOCOL_CHI_C2C_C2CPACKETIZER_HH__
#define __MEM_RUBY_PROTOCOL_CHI_C2C_C2CPACKETIZER_HH__

#include <deque>
#include <optional>
#include <vector>

#include "mem/ruby/protocol/chi/c2c/C2CContainer.hh"

namespace gem5
{

namespace ruby
{

namespace chi_c2c
{

// CHI channel class, ordered by default packing priority
// (highest first: RSP > DAT > SNP > REQ > MISC)
enum class Channel : uint8_t
{
    RSP = 0,
    DAT = 1,
    SNP = 2,
    REQ = 3,
    MISC = 4,
    NUM_CHANNELS = 5
};

// Validation result for container unpacking
enum class ContainerError : uint8_t
{
    NONE = 0,
    MSGSTART_OUT_OF_RANGE,
};

struct ValidationResult
{
    bool valid;
    ContainerError errorType;
};

// A message queued for packing into a container.
struct QueuedMsg
{
    Channel channel;
    MsgType type;
    std::vector<uint8_t> data;
};

// Packs messages from multiple channels into Format X containers.
//
// Usage:
//   packer.addMessage(msg)  -- enqueue a message
//   packer.packContainer()  -- produce one container from queued msgs
//
// The packer uses strict priority ordering to allocate granules:
// RSP > DAT > SNP > REQ > MISC (configurable via setPriority).
class ContainerPacker
{
  public:
    ContainerPacker();

    // Enqueue a message for packing.
    void addMessage(const QueuedMsg &msg);

    // Produce a container from queued messages.
    // Returns nullopt if no messages are queued.
    std::optional<C2CContainer> packContainer();

    // Check if any messages are queued.
    bool hasMessages() const;

    // Set piggybacked credit returns for the next container.
    void setCreditReturns(uint8_t req, uint8_t rsp, uint8_t dat, uint8_t snp);

    // Override default priority ordering.
    void setPriority(const std::vector<Channel> &order);

  private:
    // Per-channel FIFO queues
    static constexpr unsigned NUM_CH =
        static_cast<unsigned>(Channel::NUM_CHANNELS);
    std::array<std::deque<QueuedMsg>, NUM_CH> queues_;

    // Packing priority (index 0 = highest priority)
    std::vector<Channel> priority_;

    // Piggybacked credits for next container
    uint8_t pendingReqCredit_;
    uint8_t pendingRspCredit_;
    uint8_t pendingDatCredit_;
    uint8_t pendingSnpCredit_;
};

// Unpacks messages from a received Format X container.
class ContainerUnpacker
{
  public:
    explicit ContainerUnpacker(const C2CContainer &container);

    // Validate the container structure.
    ValidationResult validate() const;

    // Extract all messages from the container.
    // Returns a vector of PackedMsg (raw granule data per message).
    std::vector<PackedMsg> extractAll() const;

  private:
    const C2CContainer &container_;
};

} // namespace chi_c2c
} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_PROTOCOL_CHI_C2C_C2CPACKETIZER_HH__
