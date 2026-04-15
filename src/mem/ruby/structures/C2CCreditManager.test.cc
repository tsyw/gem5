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

#include <gtest/gtest.h>

#include "base/statistics.hh"
#include "base/stats/group.hh"
#include "mem/ruby/structures/C2CCreditManager.hh"
#include "sim/root.hh"

// Stub for Root::_root — required because statistics.cc references Root.
// Safe here: tests only increment stats, never trigger resolveStat().
namespace gem5
{
Root *Root::_root = nullptr;
} // namespace gem5

using namespace gem5;
using namespace gem5::ruby;

namespace
{

// Minimal stats root for testing
class TestStatsRoot : public statistics::Group
{
  public:
    TestStatsRoot() : statistics::Group(nullptr) {}
};

class C2CCreditManagerTest : public ::testing::Test
{
  protected:
    TestStatsRoot statsRoot;

    // Factory: 1 RP, 4 REQ/RP, 8 REQ shared, 8 RSP, 4 SNP,
    // 8 DAT0, 4 DAT shared, 2 MISC
    std::unique_ptr<C2CCreditManager>
    makeMgr(int numRP = 1, int reqPerRP = 4, int reqSh = 8, int rsp = 8,
            int snp = 4, int dat0 = 8, int datSh = 4, int misc = 2)
    {
        return std::make_unique<C2CCreditManager>(
            &statsRoot, numRP, reqPerRP, reqSh, rsp, snp, dat0, datSh, misc);
    }
};

// --- REQ credit tests ---

TEST_F(C2CCreditManagerTest, ReqDedicatedFirst)
{
    auto mgr = makeMgr(1, 2, 4);
    EXPECT_TRUE(mgr->hasReqCredit(0));

    // Dedicated consumed first (returns true)
    EXPECT_TRUE(mgr->consumeReqCredit(0));
    EXPECT_TRUE(mgr->consumeReqCredit(0));
    // Dedicated exhausted, shared used (returns false)
    EXPECT_FALSE(mgr->consumeReqCredit(0));
    EXPECT_TRUE(mgr->hasReqCredit(0)); // still has shared
}

TEST_F(C2CCreditManagerTest, ReqDepletion)
{
    auto mgr = makeMgr(1, 1, 1);            // 1 dedicated + 1 shared
    EXPECT_TRUE(mgr->consumeReqCredit(0));  // dedicated
    EXPECT_FALSE(mgr->consumeReqCredit(0)); // shared
    EXPECT_FALSE(mgr->hasReqCredit(0));     // both exhausted
}

TEST_F(C2CCreditManagerTest, ReqReturnRestores)
{
    auto mgr = makeMgr(1, 1, 0);
    mgr->consumeReqCredit(0);
    EXPECT_FALSE(mgr->hasReqCredit(0));
    mgr->returnReqCredit();
    // Credit is deferred until drain+apply cycle
    EXPECT_FALSE(mgr->hasReqCredit(0));
    uint8_t rq = 0, rs = 0, dt = 0, sn = 0;
    mgr->drainReturnPending(rq, rs, dt, sn);
    mgr->applyReturnCredits(rq, rs, dt, sn);
    EXPECT_TRUE(mgr->hasReqCredit(0));
    EXPECT_EQ(rq, 1);
}

// --- RSP credit tests ---

TEST_F(C2CCreditManagerTest, RspConsumeReturn)
{
    auto mgr = makeMgr(1, 1, 1, 2, 1, 1, 0, 0);
    EXPECT_TRUE(mgr->hasRspCredit());
    mgr->consumeRspCredit();
    mgr->consumeRspCredit();
    EXPECT_FALSE(mgr->hasRspCredit());
    mgr->returnRspCredit();
    EXPECT_FALSE(mgr->hasRspCredit());
    uint8_t rq = 0, rs = 0, dt = 0, sn = 0;
    mgr->drainReturnPending(rq, rs, dt, sn);
    mgr->applyReturnCredits(rq, rs, dt, sn);
    EXPECT_TRUE(mgr->hasRspCredit());
    EXPECT_EQ(rs, 1);
}

// --- SNP credit tests ---

TEST_F(C2CCreditManagerTest, SnpConsumeReturn)
{
    auto mgr = makeMgr(1, 1, 1, 1, 1, 1, 0, 0);
    EXPECT_TRUE(mgr->hasSnpCredit());
    mgr->consumeSnpCredit();
    EXPECT_FALSE(mgr->hasSnpCredit());
    mgr->returnSnpCredit();
    EXPECT_FALSE(mgr->hasSnpCredit());
    uint8_t rq = 0, rs = 0, dt = 0, sn = 0;
    mgr->drainReturnPending(rq, rs, dt, sn);
    mgr->applyReturnCredits(rq, rs, dt, sn);
    EXPECT_TRUE(mgr->hasSnpCredit());
    EXPECT_EQ(sn, 1);
}

// --- DAT credit tests ---

TEST_F(C2CCreditManagerTest, DatDedicatedThenShared)
{
    auto mgr = makeMgr(1, 1, 0, 1, 1, 2, 1, 0);
    EXPECT_TRUE(mgr->hasDatCredit());
    EXPECT_TRUE(mgr->consumeDatCredit());  // dat0
    EXPECT_TRUE(mgr->consumeDatCredit());  // dat0
    EXPECT_FALSE(mgr->consumeDatCredit()); // shared
    EXPECT_FALSE(mgr->hasDatCredit());
}

TEST_F(C2CCreditManagerTest, DatReturnRestores)
{
    auto mgr = makeMgr(1, 1, 0, 1, 1, 1, 0, 0);
    mgr->consumeDatCredit();
    EXPECT_FALSE(mgr->hasDatCredit());
    mgr->returnDatCredit();
    EXPECT_FALSE(mgr->hasDatCredit());
    uint8_t rq = 0, rs = 0, dt = 0, sn = 0;
    mgr->drainReturnPending(rq, rs, dt, sn);
    mgr->applyReturnCredits(rq, rs, dt, sn);
    EXPECT_TRUE(mgr->hasDatCredit());
    EXPECT_EQ(dt, 1);
}

// --- Stall recording ---

TEST_F(C2CCreditManagerTest, DrainClamps)
{
    auto mgr = makeMgr(1, 0, 20, 20, 20, 20, 0, 0);
    for (int i = 0; i < 20; i++) {
        mgr->returnReqCredit();
        mgr->returnRspCredit();
    }
    uint8_t rq = 0, rs = 0, dt = 0, sn = 0;
    mgr->drainReturnPending(rq, rs, dt, sn);
    EXPECT_EQ(rq, 15);
    EXPECT_EQ(rs, 15);
    // Remaining 5 still pending
    mgr->drainReturnPending(rq, rs, dt, sn);
    EXPECT_EQ(rq, 5);
    EXPECT_EQ(rs, 5);
}

TEST_F(C2CCreditManagerTest, DrainEmpty)
{
    auto mgr = makeMgr();
    uint8_t rq = 0, rs = 0, dt = 0, sn = 0;
    mgr->drainReturnPending(rq, rs, dt, sn);
    EXPECT_EQ(rq, 0);
    EXPECT_EQ(rs, 0);
    EXPECT_EQ(dt, 0);
    EXPECT_EQ(sn, 0);
}

TEST_F(C2CCreditManagerTest, RegistryLookup)
{
    auto mgr = makeMgr();
    EXPECT_EQ(C2CCreditManager::lookup(&statsRoot), mgr.get());
    mgr.reset();
    EXPECT_EQ(C2CCreditManager::lookup(&statsRoot), nullptr);
}

// --- Stall recording (original) ---

TEST_F(C2CCreditManagerTest, RecordStallPerClass)
{
    auto mgr = makeMgr();
    // Should not crash for each valid class
    mgr->recordStall(C2CCreditManager::CLS_REQ);
    mgr->recordStall(C2CCreditManager::CLS_SNP);
    mgr->recordStall(C2CCreditManager::CLS_RSP);
    mgr->recordStall(C2CCreditManager::CLS_DAT);
}

// --- Underflow protection ---

TEST_F(C2CCreditManagerTest, ReqUnderflowNoAbort)
{
    auto mgr = makeMgr(1, 0, 0);
    EXPECT_FALSE(mgr->hasReqCredit(0));
    // Consume with no credit: should warn but not crash
    EXPECT_FALSE(mgr->consumeReqCredit(0));
}

TEST_F(C2CCreditManagerTest, RspUnderflowNoAbort)
{
    auto mgr = makeMgr(1, 1, 1, 0);
    EXPECT_FALSE(mgr->hasRspCredit());
    mgr->consumeRspCredit(); // should warn, not crash
}

TEST_F(C2CCreditManagerTest, DatUnderflowNoAbort)
{
    auto mgr = makeMgr(1, 1, 0, 1, 1, 0, 0, 0);
    EXPECT_FALSE(mgr->hasDatCredit());
    EXPECT_FALSE(mgr->consumeDatCredit());
}

} // anonymous namespace
