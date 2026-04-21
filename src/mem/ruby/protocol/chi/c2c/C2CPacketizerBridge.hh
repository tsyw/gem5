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

#ifndef __MEM_RUBY_PROTOCOL_CHI_C2C_C2CPACKETIZERBRIDGE_HH__
#define __MEM_RUBY_PROTOCOL_CHI_C2C_C2CPACKETIZERBRIDGE_HH__

#include <array>
#include <deque>

#include "mem/ruby/common/Consumer.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/protocol/chi/c2c/C2CContainer.hh"
#include "mem/ruby/slicc_interface/Message.hh"
#include "mem/ruby/structures/C2CCreditManager.hh"
#include "params/C2CPacketizerBridge.hh"
#include "sim/clocked_object.hh"

namespace gem5
{

namespace ruby
{

class RubySystem;

/**
 * Unidirectional bridge that models Format X container bandwidth
 * between two CHI C2C Gateway controllers.
 *
 * Drains TX MessageBuffers, groups messages into 12-granule
 * containers (priority: RSP > DAT > SNP > REQ > MISC), and
 * delivers messages to RX MessageBuffers with configurable
 * latency.  One container is dispatched per cycle.
 *
 * Each container piggybacks deferred credit returns from the
 * TX-side controller's C2CCreditManager.  On delivery, returned
 * credits are applied to the RX-side controller's pool.
 */
class C2CPacketizerBridge : public ClockedObject, public Consumer
{
  public:
    PARAMS(C2CPacketizerBridge);
    C2CPacketizerBridge(const Params &p);

    void init() override;
    void startup() override;
    void wakeup() override;
    void print(std::ostream &out) const override;

  private:
    // Matches chi_c2c::Channel ordering
    enum ChannelIdx
    {
        CH_RSP = 0,
        CH_DAT = 1,
        CH_SNP = 2,
        CH_REQ = 3,
        CH_MISC = 4,
        NUM_CHANNELS = 5
    };

    MessageBuffer *inBuf[NUM_CHANNELS];
    MessageBuffer *outBuf[NUM_CHANNELS];

    // Per-channel queues of messages awaiting packing
    std::array<std::deque<MsgPtr>, NUM_CHANNELS> msgQueues;

    const Cycles containerLatency;
    const unsigned txqSize;    // 0 = unlimited
    unsigned bufferedGranules; // total granules currently in msgQueues
    RubySystem *const rubySystem;

    C2CCreditManager *txCreditMgr;
    C2CCreditManager *rxCreditMgr;

    // Container priority: RSP > DAT > SNP > REQ > MISC
    static constexpr int priorityOrder[NUM_CHANNELS] = {CH_RSP, CH_DAT, CH_SNP,
                                                        CH_REQ, CH_MISC};

    // Granule cost per channel (IHI0098A Table 4.4 upper bound)
    static unsigned granulesForChannel(int ch);
    void packAndDeliver();
};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_PROTOCOL_CHI_C2C_C2CPACKETIZERBRIDGE_HH__
