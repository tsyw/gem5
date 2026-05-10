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

namespace
{

QueuedMsg
makeMsg(Channel channel, MsgType type, uint8_t fill, unsigned bytes,
        uint8_t chunkValid = 0, bool shortDataUpper = false)
{
    QueuedMsg msg;
    msg.channel = channel;
    msg.type = type;
    msg.data.resize(bytes, fill);
    msg.chunkValid = chunkValid;
    msg.shortDataUpper = shortDataUpper;
    return msg;
}

} // namespace

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
    EXPECT_EQ(c.getMsgStartRaw(), (1u | (1u << 2) | (1u << 6)));
}

TEST(C2CContainer, GranulesUsedTracksOccupiedGranules)
{
    C2CContainer c;
    c.setMsgStart(1, true);
    c.setGranuleOccupied(2, true);
    c.setMsgStart(4, true);
    EXPECT_EQ(c.granulesUsed(), 3u);
    EXPECT_FLOAT_EQ(c.fullnessRatio(), 3.0f / 12.0f);
}

TEST(C2CContainer, SerializeRoundTripUsesInterleavedWireLayout)
{
    C2CContainer c;
    c.protocolHeader().containerValid = 1;
    c.protocolHeader().reqCredit = 5;
    c.protocolHeader().rspCredit = 3;
    c.protocolHeader().datCredit = 2;
    c.protocolHeader().snpCredit = 1;
    c.setMsgStart(1, true);
    c.setMsgStart(4, true);
    c.setGranuleOccupied(1, true);
    c.setGranuleOccupied(4, true);

    uint8_t *g1 = c.granuleData(1);
    g1[0] = wireCodeForMsgType(MsgType::Resp);
    g1[1] = 0xAB;

    uint8_t *g4 = c.granuleData(4);
    g4[0] = wireCodeForMsgType(MsgType::Resp2);
    g4[1] = 0xCD;

    uint8_t buf[CONTAINER_SIZE];
    c.serialize(buf);

    EXPECT_EQ(buf[2], wireCodeForMsgType(MsgType::Resp));
    EXPECT_EQ(buf[3], 0xAB);
    EXPECT_EQ(buf[62], 0x35);
    EXPECT_EQ(buf[63], 0x12);
    EXPECT_EQ(buf[64], 0x09);
    EXPECT_EQ(buf[66], wireCodeForMsgType(MsgType::Resp2));
    EXPECT_EQ(buf[67], 0xCD);

    C2CContainer c2;
    c2.deserialize(buf);
    EXPECT_NE(c2.protocolHeader().containerValid, 0);
    EXPECT_EQ(c2.protocolHeader().reqCredit, 5);
    EXPECT_EQ(c2.protocolHeader().rspCredit, 3);
    EXPECT_EQ(c2.protocolHeader().datCredit, 2);
    EXPECT_EQ(c2.protocolHeader().snpCredit, 1);
    EXPECT_TRUE(c2.msgStart(1));
    EXPECT_TRUE(c2.msgStart(4));
    EXPECT_EQ(c2.granuleData(1)[0], wireCodeForMsgType(MsgType::Resp));
    EXPECT_EQ(c2.granuleData(1)[1], 0xAB);
    EXPECT_EQ(c2.granuleData(4)[0], wireCodeForMsgType(MsgType::Resp2));
    EXPECT_EQ(c2.granuleData(4)[1], 0xCD);
    EXPECT_EQ(c2.granulesUsed(), 2u);
}

TEST(MsgType, WireSizesAndGranuleCounts)
{
    EXPECT_EQ(granulesForMsgType(MsgType::Resp), 1u);
    EXPECT_EQ(granulesForMsgType(MsgType::Resp2), 1u);
    EXPECT_EQ(granulesForMsgType(MsgType::ReqS), 2u);
    EXPECT_EQ(granulesForMsgType(MsgType::DataS), 3u);
    EXPECT_EQ(wireBytesForMsgType(MsgType::Resp), 10u);
    EXPECT_EQ(wireBytesForMsgType(MsgType::Resp2), 20u);
    EXPECT_EQ(wireBytesForMsgType(MsgType::DataS), 60u);
    EXPECT_EQ(responseSlotUnitsForMsgType(MsgType::Resp), 1u);
    EXPECT_EQ(responseSlotUnitsForMsgType(MsgType::Resp2), 2u);
}

TEST(ContainerPacker, EmptyPacker)
{
    ContainerPacker packer;
    EXPECT_FALSE(packer.hasMessages());
    EXPECT_FALSE(packer.packContainer().has_value());
}

TEST(ContainerPacker, CreditOnlyContainer)
{
    ContainerPacker packer;
    packer.setCreditReturns(3, 2, 1, 0);
    auto result = packer.packContainer();
    ASSERT_TRUE(result.has_value());

    const auto &c = result.value();
    EXPECT_NE(c.protocolHeader().containerValid, 0);
    EXPECT_EQ(c.protocolHeader().reqCredit, 3);
    EXPECT_EQ(c.protocolHeader().rspCredit, 2);
    EXPECT_EQ(c.protocolHeader().datCredit, 1);
    EXPECT_EQ(c.protocolHeader().snpCredit, 0);
    EXPECT_EQ(c.granulesUsed(), 0u);
    EXPECT_EQ(c.getMsgStartRaw(), 0u);
}

TEST(ContainerPacker, SingleResponse)
{
    ContainerPacker packer;
    packer.addMessage(makeMsg(Channel::RSP, MsgType::Resp, 0x42, 4));

    auto result = packer.packContainer();
    ASSERT_TRUE(result.has_value());

    const auto &c = result.value();
    EXPECT_TRUE(c.msgStart(1));
    EXPECT_FALSE(c.msgStart(2));
    EXPECT_EQ(c.granuleData(1)[0], wireCodeForMsgType(MsgType::Resp));
    EXPECT_EQ(c.granuleData(1)[1], 0x42);
    EXPECT_EQ(c.granulesUsed(), 1u);
}

TEST(ContainerPacker, TwoResponsesShareGranule)
{
    ContainerPacker packer;
    packer.addMessage(makeMsg(Channel::RSP, MsgType::Resp, 0xA1, 2));
    packer.addMessage(makeMsg(Channel::RSP, MsgType::Resp, 0xB2, 2));

    auto result = packer.packContainerDetailed();
    ASSERT_TRUE(result.has_value());

    const auto &c = result->container;
    EXPECT_EQ(result->packedMessages.size(), 2u);
    EXPECT_TRUE(c.msgStart(1));
    EXPECT_EQ(c.granulesUsed(), 1u);
    EXPECT_EQ(c.granuleData(1)[0], wireCodeForMsgType(MsgType::Resp));
    EXPECT_EQ(c.granuleData(1)[1], 0xA1);
    EXPECT_EQ(c.granuleData(1)[10], wireCodeForMsgType(MsgType::Resp));
    EXPECT_EQ(c.granuleData(1)[11], 0xB2);
}

TEST(ContainerPacker, Resp2CountsAsTwoSlotsAndPreservesFifo)
{
    ContainerPacker packer;
    packer.addMessage(makeMsg(Channel::RSP, MsgType::Resp, 0x11, 1));
    packer.addMessage(makeMsg(Channel::RSP, MsgType::Resp2, 0x22, 4));
    packer.addMessage(makeMsg(Channel::RSP, MsgType::Resp, 0x33, 1));

    auto result = packer.packContainerDetailed();
    ASSERT_TRUE(result.has_value());

    const auto &c = result->container;
    EXPECT_EQ(result->packedMessages.size(), 3u);
    EXPECT_EQ(result->packedMessages[0].type, MsgType::Resp);
    EXPECT_EQ(result->packedMessages[1].type, MsgType::Resp2);
    EXPECT_EQ(result->packedMessages[2].type, MsgType::Resp);
    EXPECT_TRUE(c.msgStart(1));
    EXPECT_TRUE(c.msgStart(2));
    EXPECT_TRUE(c.msgStart(3));
    EXPECT_EQ(c.granuleData(1)[0], wireCodeForMsgType(MsgType::Resp));
    EXPECT_EQ(c.granuleData(2)[0], wireCodeForMsgType(MsgType::Resp2));
    EXPECT_EQ(c.granuleData(3)[0], wireCodeForMsgType(MsgType::Resp));
}

TEST(ContainerPacker, FullContainer16Responses)
{
    ContainerPacker packer;
    for (unsigned i = 0; i < 16; ++i) {
        packer.addMessage(
            makeMsg(Channel::RSP, MsgType::Resp, static_cast<uint8_t>(i), 1));
    }

    auto result = packer.packContainer();
    ASSERT_TRUE(result.has_value());

    const auto &c = result.value();
    EXPECT_EQ(c.granulesUsed(), 8u);
    for (unsigned g : {1u, 2u, 4u, 5u, 7u, 8u, 10u, 11u}) {
        EXPECT_TRUE(c.msgStart(g));
    }
}

TEST(ContainerPacker, ShortDataUsesAddr5SelectedHalf)
{
    ContainerPacker packer;
    QueuedMsg dat = makeMsg(Channel::DAT, MsgType::DataS, 0, 32, 0x2, true);
    for (unsigned i = 0; i < 32; ++i) {
        dat.data[i] = static_cast<uint8_t>(0x80 + i);
    }
    packer.addMessage(dat);

    auto packed = packer.packContainer();
    ASSERT_TRUE(packed.has_value());

    ContainerUnpacker unpacker(*packed);
    ASSERT_TRUE(unpacker.validate().valid);
    auto messages = unpacker.extractAll();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].type, MsgType::DataS);
    EXPECT_EQ(messages[0].chunkValid, 0x2);
    for (unsigned i = 0; i < 32; ++i) {
        EXPECT_EQ(messages[0].dataField[i], 0u);
        EXPECT_EQ(messages[0].dataField[32 + i],
                  static_cast<uint8_t>(0x80 + i));
    }
}

TEST(ContainerPacker, LargeMessageDoesNotFit)
{
    ContainerPacker packer;
    for (unsigned i = 0; i < 6; ++i) {
        packer.addMessage(makeMsg(Channel::REQ, MsgType::ReqS, 0, 8));
    }
    packer.addMessage(makeMsg(Channel::DAT, MsgType::DataL, 0xFF, 64, 0x3));

    auto first = packer.packContainer();
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(packer.hasMessages());
}

TEST(ContainerUnpacker, ValidateEmpty)
{
    C2CContainer c;
    ContainerUnpacker unpacker(c);
    EXPECT_TRUE(unpacker.validate().valid);
}

TEST(ContainerUnpacker, ValidateOutOfRange)
{
    C2CContainer c;
    c.setMsgStartRaw(1u << 12);
    ContainerUnpacker unpacker(c);
    auto result = unpacker.validate();
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.errorType, ContainerError::MSGSTART_OUT_OF_RANGE);
}

TEST(ContainerUnpacker, ValidateRejectsStrayDataWithoutMsgStart)
{
    C2CContainer c;
    c.granuleData(2)[1] = 0x5a;
    ContainerUnpacker unpacker(c);
    auto result = unpacker.validate();
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.errorType, ContainerError::NONZERO_GRANULE_WITHOUT_START);
}

TEST(ContainerUnpacker, ValidateRejectsMsgStartInsideBody)
{
    C2CContainer c;
    c.setMsgStart(1, true);
    c.setMsgStart(2, true);
    c.granuleData(1)[0] = wireCodeForMsgType(MsgType::ReqS);
    c.granuleData(2)[0] = wireCodeForMsgType(MsgType::Resp);
    ContainerUnpacker unpacker(c);
    auto result = unpacker.validate();
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.errorType, ContainerError::UNEXPECTED_MSGSTART_IN_BODY);
}

TEST(ContainerUnpacker, ValidateRejectsTooManyResponsesInGroup)
{
    C2CContainer c;
    c.setMsgStart(1, true);
    c.setGranuleOccupied(1, true);
    c.granuleData(1)[0] = wireCodeForMsgType(MsgType::Resp);
    c.granuleData(1)[10] = wireCodeForMsgType(MsgType::Resp);

    c.setMsgStart(2, true);
    c.setGranuleOccupied(2, true);
    c.granuleData(2)[0] = wireCodeForMsgType(MsgType::Resp);
    c.granuleData(2)[10] = wireCodeForMsgType(MsgType::Resp);

    c.setMsgStart(3, true);
    c.setGranuleOccupied(3, true);
    c.granuleData(3)[0] = wireCodeForMsgType(MsgType::Resp);

    ContainerUnpacker unpacker(c);
    auto result = unpacker.validate();
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.errorType, ContainerError::TOO_MANY_RESPONSES_IN_GROUP);
}

TEST(ContainerUnpacker, ValidateRejectsInvalidChunkValid)
{
    C2CContainer c;
    c.setMsgStart(1, true);
    c.setGranuleOccupied(1, true);
    c.setGranuleOccupied(2, true);
    c.setGranuleOccupied(3, true);
    c.granuleData(1)[0] = wireCodeForMsgType(MsgType::DataS);
    c.granuleData(1)[1] = 0x3;

    ContainerUnpacker unpacker(c);
    auto result = unpacker.validate();
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.errorType, ContainerError::INVALID_CHUNK_VALID);
}

TEST(ContainerUnpacker, ExtractMultipleMessages)
{
    ContainerPacker packer;
    packer.addMessage(makeMsg(Channel::RSP, MsgType::Resp, 0x11, 2));
    packer.addMessage(makeMsg(Channel::DAT, MsgType::DataS, 0x22, 32, 0x1));
    packer.addMessage(makeMsg(Channel::REQ, MsgType::ReqS, 0x33, 8));

    auto packed = packer.packContainer();
    ASSERT_TRUE(packed.has_value());

    ContainerUnpacker unpacker(*packed);
    ASSERT_TRUE(unpacker.validate().valid);
    auto messages = unpacker.extractAll();
    ASSERT_EQ(messages.size(), 3u);

    EXPECT_EQ(messages[0].type, MsgType::Resp);
    EXPECT_EQ(messages[0].data[1], 0x11);
    EXPECT_EQ(messages[1].type, MsgType::DataS);
    EXPECT_EQ(messages[1].chunkValid, 0x1);
    EXPECT_EQ(messages[1].dataField[0], 0x22);
    EXPECT_EQ(messages[1].dataField[31], 0x22);
    EXPECT_EQ(messages[1].dataField[32], 0u);
    EXPECT_EQ(messages[2].type, MsgType::ReqS);
    EXPECT_EQ(messages[2].numGranules, 2u);
}
