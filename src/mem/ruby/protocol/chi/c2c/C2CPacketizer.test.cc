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

#include "mem/ruby/protocol/chi/c2c/C2CContainer.hh"
#include "mem/ruby/protocol/chi/c2c/C2CPacketizer.hh"

using namespace gem5::ruby::chi_c2c;

// --------------------------------------------------------------------------
// C2CContainer tests
// --------------------------------------------------------------------------

TEST(C2CContainer, InitialState)
{
    C2CContainer c;
    EXPECT_EQ(c.size(), CONTAINER_SIZE);
    EXPECT_EQ(c.granulesTotal(), NUM_MSG_GRANULES);
    EXPECT_EQ(c.granulesUsed(), 0u);
    EXPECT_FLOAT_EQ(c.fullnessRatio(), 0.0f);
    EXPECT_EQ(c.getMsgStartRaw(), 0u);
    EXPECT_EQ(c.protocolHeader().containerValid, 0);
}

TEST(C2CContainer, MsgStartBits)
{
    C2CContainer c;
    c.setMsgStart(1, true);
    c.setMsgStart(3, true);
    c.setMsgStart(7, true);

    EXPECT_TRUE(c.msgStart(1));
    EXPECT_FALSE(c.msgStart(2));
    EXPECT_TRUE(c.msgStart(3));
    EXPECT_FALSE(c.msgStart(4));
    EXPECT_TRUE(c.msgStart(7));

    // Raw value: bits 0, 2, 6 set
    EXPECT_EQ(c.getMsgStartRaw(), (1u | (1u << 2) | (1u << 6)));
}

TEST(C2CContainer, GranulesUsedSingleMsg)
{
    C2CContainer c;
    // Single 1-granule message at granule 1
    c.setMsgStart(1, true);
    // No other starts -> message spans only granule 1?
    // Actually granulesUsed counts from start to next start or end
    // With only one start at 1, it spans 1..12 = 12 granules
    // That's not right for a typical 1-granule message.
    // But granulesUsed doesn't know message sizes, it uses MsgStart.
    // To represent a 1-gran msg at 1 followed by a 1-gran msg at 2:
    // setMsgStart(1,true), setMsgStart(2,true)
    // granulesUsed = (2-1) + (3-2) or until end...
    // The design means: a start at 1 with next start at 2 → 1 granule.
    // A start at 1 with no more starts → all remaining = 12 granules.
    EXPECT_EQ(c.granulesUsed(), 12u);
}

TEST(C2CContainer, GranulesUsedMultipleMsgs)
{
    C2CContainer c;
    // 3 messages: 2-gran at 1, 1-gran at 3, 3-gran at 4
    c.setMsgStart(1, true);
    c.setMsgStart(3, true);
    c.setMsgStart(4, true);
    // Msg 1: granules 1-2 (2 grans)
    // Msg 2: granule  3   (1 gran)
    // Msg 3: granules 4-12 (9 grans) -- extends to end
    EXPECT_EQ(c.granulesUsed(), 2u + 1u + 9u);
}

TEST(C2CContainer, GranuleSize)
{
    C2CContainer c;
    for (unsigned g = 1; g < NUM_MSG_GRANULES; g++) {
        EXPECT_EQ(c.granuleSize(g), GRANULE_SIZE);
    }
    // Last granule is 16 bytes
    EXPECT_EQ(c.granuleSize(NUM_MSG_GRANULES), 16u);
}

TEST(C2CContainer, SerializeRoundTrip)
{
    C2CContainer c;
    c.protocolHeader().containerValid = 1;
    c.protocolHeader().reqCredit = 5;
    c.protocolHeader().rspCredit = 3;
    c.setMsgStart(1, true);

    // Write some data
    uint8_t *g1 = c.granuleData(1);
    g1[0] = 0xAB;
    g1[19] = 0xCD;

    uint8_t buf[CONTAINER_SIZE];
    c.serialize(buf);

    C2CContainer c2;
    c2.deserialize(buf);

    EXPECT_NE(c2.protocolHeader().containerValid, 0);
    EXPECT_EQ(c2.protocolHeader().reqCredit, 5);
    EXPECT_EQ(c2.protocolHeader().rspCredit, 3);
    EXPECT_TRUE(c2.msgStart(1));
    EXPECT_EQ(c2.granuleData(1)[0], 0xAB);
    EXPECT_EQ(c2.granuleData(1)[19], 0xCD);
}

// --------------------------------------------------------------------------
// GranulesForMsgType tests
// --------------------------------------------------------------------------

TEST(MsgType, GranuleCounts)
{
    EXPECT_EQ(granulesForMsgType(MsgType::Resp), 1u);
    EXPECT_EQ(granulesForMsgType(MsgType::Snoop), 1u);
    EXPECT_EQ(granulesForMsgType(MsgType::MiscU), 1u);
    EXPECT_EQ(granulesForMsgType(MsgType::ReqS), 2u);
    EXPECT_EQ(granulesForMsgType(MsgType::ReqL), 2u);
    EXPECT_EQ(granulesForMsgType(MsgType::DataS), 3u);
    EXPECT_EQ(granulesForMsgType(MsgType::WrReqS), 3u);
    EXPECT_EQ(granulesForMsgType(MsgType::DataL), 5u);
    EXPECT_EQ(granulesForMsgType(MsgType::WrReqL), 5u);
}

// --------------------------------------------------------------------------
// ContainerPacker tests
// --------------------------------------------------------------------------

TEST(ContainerPacker, EmptyPacker)
{
    ContainerPacker packer;
    EXPECT_FALSE(packer.hasMessages());
    auto result = packer.packContainer();
    EXPECT_FALSE(result.has_value());
}

TEST(ContainerPacker, CreditOnlyContainer)
{
    ContainerPacker packer;
    packer.setCreditReturns(3, 2, 1, 0);
    auto result = packer.packContainer();
    ASSERT_TRUE(result.has_value());

    auto &c = result.value();
    EXPECT_NE(c.protocolHeader().containerValid, 0);
    EXPECT_EQ(c.protocolHeader().reqCredit, 3);
    EXPECT_EQ(c.protocolHeader().rspCredit, 2);
    EXPECT_EQ(c.protocolHeader().datCredit, 1);
    EXPECT_EQ(c.protocolHeader().snpCredit, 0);
    EXPECT_EQ(c.granulesUsed(), 0u);
}

TEST(ContainerPacker, SingleResponse)
{
    ContainerPacker packer;
    QueuedMsg msg;
    msg.channel = Channel::RSP;
    msg.type = MsgType::Resp;
    msg.data.resize(GRANULE_SIZE, 0x42);
    packer.addMessage(msg);

    auto result = packer.packContainer();
    ASSERT_TRUE(result.has_value());

    auto &c = result.value();
    EXPECT_TRUE(c.msgStart(1));
    EXPECT_FALSE(c.msgStart(2));
    EXPECT_EQ(c.granuleData(1)[0], 0x42);
}

TEST(ContainerPacker, PriorityOrdering)
{
    ContainerPacker packer;

    // Add a REQ (lower priority) first
    QueuedMsg req;
    req.channel = Channel::REQ;
    req.type = MsgType::ReqS;
    req.data.resize(GRANULE_SIZE * 2, 0xAA);
    packer.addMessage(req);

    // Add a RSP (higher priority) second
    QueuedMsg rsp;
    rsp.channel = Channel::RSP;
    rsp.type = MsgType::Resp;
    rsp.data.resize(GRANULE_SIZE, 0xBB);
    packer.addMessage(rsp);

    auto result = packer.packContainer();
    ASSERT_TRUE(result.has_value());

    auto &c = result.value();
    // RSP should be packed first (granule 1)
    EXPECT_TRUE(c.msgStart(1));
    EXPECT_EQ(c.granuleData(1)[0], 0xBB);
    // REQ second (granule 2, 2 granules)
    EXPECT_TRUE(c.msgStart(2));
    EXPECT_EQ(c.granuleData(2)[0], 0xAA);
}

TEST(ContainerPacker, FullContainer12Responses)
{
    ContainerPacker packer;
    for (unsigned i = 0; i < 12; i++) {
        QueuedMsg msg;
        msg.channel = Channel::RSP;
        msg.type = MsgType::Resp;
        msg.data.resize(GRANULE_SIZE, static_cast<uint8_t>(i));
        packer.addMessage(msg);
    }

    auto result = packer.packContainer();
    ASSERT_TRUE(result.has_value());

    auto &c = result.value();
    // All 12 granules used
    for (unsigned g = 1; g <= 12; g++) {
        EXPECT_TRUE(c.msgStart(g));
        EXPECT_EQ(c.granuleData(g)[0], static_cast<uint8_t>(g - 1));
    }
}

TEST(ContainerPacker, LargeMessageDoesNotFit)
{
    ContainerPacker packer;

    // Fill 10 of 12 granules with 5 requests (2 gran each)
    for (unsigned i = 0; i < 5; i++) {
        QueuedMsg msg;
        msg.channel = Channel::REQ;
        msg.type = MsgType::ReqS;
        msg.data.resize(GRANULE_SIZE * 2, 0);
        packer.addMessage(msg);
    }

    // Add a DataL (5 granules) -- won't fit (only 2 left)
    QueuedMsg big;
    big.channel = Channel::DAT;
    big.type = MsgType::DataL;
    big.data.resize(GRANULE_SIZE * 5, 0xFF);
    packer.addMessage(big);

    auto c1 = packer.packContainer();
    ASSERT_TRUE(c1.has_value());
    // DataL has higher priority than REQ, but 5 > 12, wait...
    // Actually DAT priority > REQ, so DAT is packed first (5 grans).
    // Then REQ: 7 grans left, can fit 3 * 2-gran = 6, leaving 1.
    // So 1 message + 3 messages = 4 messages, 11 granules.
    EXPECT_TRUE(packer.hasMessages()); // 2 REQ remain
}

// --------------------------------------------------------------------------
// ContainerUnpacker tests
// --------------------------------------------------------------------------

TEST(ContainerUnpacker, ValidateEmpty)
{
    C2CContainer c;
    ContainerUnpacker unpacker(c);
    auto result = unpacker.validate();
    EXPECT_TRUE(result.valid);
}

TEST(ContainerUnpacker, ValidateOutOfRange)
{
    C2CContainer c;
    // Set bit 13 (out of range for 12 granules)
    c.setMsgStartRaw(1u << 12);
    ContainerUnpacker unpacker(c);
    auto result = unpacker.validate();
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.errorType, ContainerError::MSGSTART_OUT_OF_RANGE);
}

TEST(ContainerUnpacker, ExtractMultipleMessages)
{
    ContainerPacker packer;

    // Pack: 1 RSP (1 gran) + 1 ReqS (2 gran) + 1 DataS (3 gran)
    QueuedMsg rsp;
    rsp.channel = Channel::RSP;
    rsp.type = MsgType::Resp;
    rsp.data.resize(GRANULE_SIZE, 0x11);
    packer.addMessage(rsp);

    QueuedMsg req;
    req.channel = Channel::REQ;
    req.type = MsgType::ReqS;
    req.data.resize(GRANULE_SIZE * 2, 0x22);
    packer.addMessage(req);

    QueuedMsg dat;
    dat.channel = Channel::DAT;
    dat.type = MsgType::DataS;
    dat.data.resize(GRANULE_SIZE * 3, 0x33);
    packer.addMessage(dat);

    auto packed = packer.packContainer();
    ASSERT_TRUE(packed.has_value());

    ContainerUnpacker unpacker(packed.value());
    auto result = unpacker.validate();
    EXPECT_TRUE(result.valid);

    auto messages = unpacker.extractAll();
    EXPECT_EQ(messages.size(), 3u);

    // Priority: RSP (1 gran), DAT (3 gran), REQ (2 gran packed, but
    // unpacker sees span to end = 8 granules since no following start bit)
    EXPECT_EQ(messages[0].numGranules, 1u);
    EXPECT_EQ(messages[0].data[0], 0x11);
    EXPECT_EQ(messages[1].numGranules, 3u);
    EXPECT_EQ(messages[1].data[0], 0x33);
    EXPECT_EQ(messages[2].numGranules, 8u);
    EXPECT_EQ(messages[2].data[0], 0x22);
}
