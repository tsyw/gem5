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

#ifndef __MEM_RUBY_STRUCTURES_C2CCREDITMANAGER_HH__
#define __MEM_RUBY_STRUCTURES_C2CCREDITMANAGER_HH__

#include <algorithm>
#include <cassert>

#include "base/logging.hh"
#include "base/statistics.hh"
#include "base/stats/group.hh"

namespace gem5
{
namespace ruby
{

/**
 * C2CCreditManager — Per-message-class flow control for CHI C2C.
 *
 * Tracks protocol-level credits per message class (REQ, RSP, SNP, DAT)
 * with sub-pools per ARM IHI 0098A Section B5:
 *   - REQ: per-RP dedicated + shared pool
 *   - DAT: dat0 (non-WritePush) + shared pool
 *   - RSP, SNP: single pool each
 *
 * Phase 3: single RP (RP=0), no WritePush, instant credit return.
 * Return credits are replenished directly (no ProtHdr piggybacking
 * until Phase 7 packetization is implemented).
 */
class C2CCreditManager
{
  public:
    // Message class indices (match VNET order)
    static constexpr int CLS_REQ = 0;
    static constexpr int CLS_SNP = 1;
    static constexpr int CLS_RSP = 2;
    static constexpr int CLS_DAT = 3;
    static constexpr int NUM_CLS = 4;

    static constexpr int MAX_RP = 8;

    C2CCreditManager(statistics::Group *parent, int num_rp,
                     int init_req_per_rp, int init_req_shared, int init_rsp,
                     int init_snp, int init_dat0, int init_dat_shared,
                     int /* init_misc -- reserved for Phase 5 */)
        : m_reqSh(init_req_shared),
          m_rsp(init_rsp),
          m_snp(init_snp),
          m_dat0(init_dat0),
          m_datSh(init_dat_shared),
          m_stats(parent)
    {
        assert(num_rp >= 1 && num_rp <= MAX_RP);
        for (int i = 0; i < MAX_RP; i++) {
            m_reqRP[i] = (i < num_rp) ? init_req_per_rp : 0;
        }
    }

    // --- Credit availability queries (called from SLICC) ---

    /** Check if REQ credit is available for the given RP. */
    bool
    hasReqCredit(int rp) const
    {
        return (m_reqRP[rp] > 0) || (m_reqSh > 0);
    }

    /** Check if RSP credit is available. */
    bool
    hasRspCredit() const
    {
        return m_rsp > 0;
    }

    /** Check if SNP credit is available. */
    bool
    hasSnpCredit() const
    {
        return m_snp > 0;
    }

    /** Check if DAT credit is available (dat0 or shared). */
    bool
    hasDatCredit() const
    {
        return (m_dat0 > 0) || (m_datSh > 0);
    }

    // --- Credit consumption (called from SLICC on outbound enqueue) ---

    /**
     * Consume one REQ credit for the given RP.
     * Returns true if dedicated RP credit was used, false if shared.
     * Caller must check hasReqCredit() first.
     */
    bool
    consumeReqCredit(int rp)
    {
        if (m_reqRP[rp] > 0) {
            m_reqRP[rp]--;
            m_stats.reqCreditsConsumed++;
            return true; // used dedicated
        } else if (m_reqSh > 0) {
            m_reqSh--;
            m_stats.reqCreditsConsumed++;
            m_stats.reqSharedUsed++;
            return false; // used shared
        }
        // Underflow protection
        m_stats.creditUnderflow++;
        warn_once("C2CCreditManager: REQ credit underflow for RP %d\n", rp);
        return false;
    }

    /** Consume one RSP credit. */
    void
    consumeRspCredit()
    {
        if (m_rsp > 0) {
            m_rsp--;
            m_stats.rspCreditsConsumed++;
        } else {
            m_stats.creditUnderflow++;
            warn_once("C2CCreditManager: RSP credit underflow\n");
        }
    }

    /** Consume one SNP credit. */
    void
    consumeSnpCredit()
    {
        if (m_snp > 0) {
            m_snp--;
            m_stats.snpCreditsConsumed++;
        } else {
            m_stats.creditUnderflow++;
            warn_once("C2CCreditManager: SNP credit underflow\n");
        }
    }

    /** Consume one DAT credit (dat0, fallback to shared). */
    bool
    consumeDatCredit()
    {
        if (m_dat0 > 0) {
            m_dat0--;
            m_stats.datCreditsConsumed++;
            return true;
        } else if (m_datSh > 0) {
            m_datSh--;
            m_stats.datCreditsConsumed++;
            return false;
        }
        m_stats.creditUnderflow++;
        warn_once("C2CCreditManager: DAT credit underflow\n");
        return false;
    }

    // --- Credit return (called from SLICC on inbound dequeue) ---
    // Phase 3: credits are returned directly to the available pool
    // (instant return). Phase 7 will defer returns to ProtHdr
    // piggybacking via m_returnPending + drainReturnPending().

    void
    returnReqCredit()
    {
        // Return to RP0 dedicated pool (Phase 3: single RP)
        m_reqRP[0]++;
        m_stats.reqCreditsReturned++;
    }

    void
    returnRspCredit()
    {
        m_rsp++;
        m_stats.rspCreditsReturned++;
    }

    void
    returnSnpCredit()
    {
        m_snp++;
        m_stats.snpCreditsReturned++;
    }

    void
    returnDatCredit()
    {
        m_dat0++;
        m_stats.datCreditsReturned++;
    }

    void
    recordStall(int cls)
    {
        assert(cls >= 0 && cls < NUM_CLS);
        m_stats.creditStallsPerClass[cls]++;
    }

  private:
    // REQ credits: per-RP dedicated + shared
    int m_reqRP[MAX_RP];
    int m_reqSh;

    // Single-pool classes
    int m_rsp;
    int m_snp;

    // DAT credits
    int m_dat0;
    int m_datSh;

    struct C2CCreditStats : public statistics::Group
    {
        C2CCreditStats(statistics::Group *parent)
            : statistics::Group(parent, "c2cCredits"),
              ADD_STAT(reqCreditsConsumed, "REQ credits consumed"),
              ADD_STAT(rspCreditsConsumed, "RSP credits consumed"),
              ADD_STAT(snpCreditsConsumed, "SNP credits consumed"),
              ADD_STAT(datCreditsConsumed, "DAT credits consumed"),
              ADD_STAT(reqCreditsReturned, "REQ credits returned"),
              ADD_STAT(rspCreditsReturned, "RSP credits returned"),
              ADD_STAT(snpCreditsReturned, "SNP credits returned"),
              ADD_STAT(datCreditsReturned, "DAT credits returned"),
              ADD_STAT(reqSharedUsed, "REQ shared pool credits used"),
              ADD_STAT(creditUnderflow,
                       "Credit underflow events (bug indicator)"),
              ADD_STAT(creditStallsPerClass,
                       "Outbound stalls per message class (REQ/SNP/RSP/DAT)")
        {
            creditStallsPerClass.init(NUM_CLS);
            creditStallsPerClass.subname(CLS_REQ, "REQ");
            creditStallsPerClass.subname(CLS_SNP, "SNP");
            creditStallsPerClass.subname(CLS_RSP, "RSP");
            creditStallsPerClass.subname(CLS_DAT, "DAT");
        }

        statistics::Scalar reqCreditsConsumed;
        statistics::Scalar rspCreditsConsumed;
        statistics::Scalar snpCreditsConsumed;
        statistics::Scalar datCreditsConsumed;
        statistics::Scalar reqCreditsReturned;
        statistics::Scalar rspCreditsReturned;
        statistics::Scalar snpCreditsReturned;
        statistics::Scalar datCreditsReturned;
        statistics::Scalar reqSharedUsed;
        statistics::Scalar creditUnderflow;
        statistics::Vector creditStallsPerClass;
    } m_stats;
};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_STRUCTURES_C2CCREDITMANAGER_HH__
