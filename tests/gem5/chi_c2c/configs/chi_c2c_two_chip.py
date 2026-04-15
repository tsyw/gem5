# Copyright (c) 2025 The gem5 Contributors
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""
Two-chip CHI C2C test configuration.

Creates two CHI sub-systems (Chip 0 and Chip 1), each with:
  - 1 RNF (request node with L1/L2 caches)
  - 1 HNF (home node with L3 cache)
  - 1 SNF (memory controller)
  - 1 MN  (misc node for DVM)
  - 1 C2CG (chip-to-chip gateway)

The C2CGs are wired together via wireC2CLink().

Address space split:
  Chip 0 HNF owns [0x00000000, 0x80000000)   (lower 2 GiB)
  Chip 1 HNF owns [0x80000000, 0x100000000)  (upper 2 GiB)

Each RNF routes all requests to both local HNF and C2CG.
The C2CG forwards remote-addressed requests across the link.

NOTE: This config is a scaffold for T-020 integration tests.
It requires packetizer integration (T-017) before the C2C link
can carry actual coherent traffic.
"""

import argparse
import os
import sys

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.util import addToPath

m5.util.addToPath(os.path.join(m5.util.repoPath(), "configs"))
from common import Options
from ruby import Ruby
from ruby.CHI_config import (
    CHI_HNF,
    CHI_MN,
    CHI_RNF,
    CHI_SNF_MainMem,
    NoC_Params,
    Versions,
)

# Conditional import — C2C classes may not be available in all builds
try:
    from ruby.CHI_C2C_config import (
        CHI_C2CG,
        wireC2CLink,
    )
except ImportError:
    CHI_C2CG = None
    wireC2CLink = None


def build_two_chip_system(
    ruby_system,
    system,
    options,
    chip0_range=None,
    chip1_range=None,
):
    """
    Build a two-chip CHI system with C2C gateways.

    Returns:
        (network_nodes, network_cntrls, all_cntrls,
         cpu_sequencers, mem_cntrls, c2cg_pair)
    """
    if CHI_C2CG is None:
        m5.fatal("CHI C2C config classes not available in this build")

    if chip0_range is None:
        chip0_range = AddrRange(0, size="2GiB")
    if chip1_range is None:
        chip1_range = AddrRange("2GiB", size="2GiB")

    full_range = [chip0_range, chip1_range]
    cache_line_size = system.cache_line_size.value
    params = NoC_Params

    class HNFCache(RubyCache):
        dataAccessLatency = 10
        tagAccessLatency = 2
        size = "256KiB"
        assoc = 8

    network_nodes = []
    network_cntrls = []
    all_cntrls = []
    cpu_sequencers = []
    mem_cntrls = []

    # --- Chip 0 ---
    rnf0 = CHI_RNF(
        system.cpu[0],
        ruby_system,
        options.l1i_size,
        options.l1d_size,
        options.l2_size,
        None,
    )
    cpu_sequencers.extend(rnf0.getSequencers())
    all_cntrls.extend(rnf0.getAllControllers())
    network_nodes.append(rnf0)
    network_cntrls.extend(rnf0.getNetworkSideControllers())

    CHI_HNF.createAddrRanges([chip0_range], cache_line_size, [0])
    hnf0 = CHI_HNF(0, ruby_system, HNFCache, None)
    all_cntrls.extend(hnf0.getAllControllers())
    network_nodes.append(hnf0)
    network_cntrls.extend(hnf0.getNetworkSideControllers())

    snf0 = CHI_SNF_MainMem(ruby_system, None, None)
    all_cntrls.extend(snf0.getAllControllers())
    network_nodes.append(snf0)
    network_cntrls.extend(snf0.getNetworkSideControllers())
    mem_cntrls.extend(snf0.getAllControllers())

    all_rnf0 = rnf0.getAllControllers()
    mn0 = CHI_MN(ruby_system, all_rnf0)
    all_cntrls.extend(mn0.getAllControllers())
    network_nodes.append(mn0)
    network_cntrls.extend(mn0.getNetworkSideControllers())

    c2cg0 = CHI_C2CG(ruby_system, full_range)
    all_cntrls.extend(c2cg0.getAllControllers())
    network_nodes.append(c2cg0)
    network_cntrls.extend(c2cg0.getNetworkSideControllers())

    # --- Chip 1 ---
    rnf1 = CHI_RNF(
        system.cpu[1],
        ruby_system,
        options.l1i_size,
        options.l1d_size,
        options.l2_size,
        None,
    )
    cpu_sequencers.extend(rnf1.getSequencers())
    all_cntrls.extend(rnf1.getAllControllers())
    network_nodes.append(rnf1)
    network_cntrls.extend(rnf1.getNetworkSideControllers())

    CHI_HNF.createAddrRanges([chip1_range], cache_line_size, [1])
    hnf1 = CHI_HNF(1, ruby_system, HNFCache, None)
    all_cntrls.extend(hnf1.getAllControllers())
    network_nodes.append(hnf1)
    network_cntrls.extend(hnf1.getNetworkSideControllers())

    snf1 = CHI_SNF_MainMem(ruby_system, None, None)
    all_cntrls.extend(snf1.getAllControllers())
    network_nodes.append(snf1)
    network_cntrls.extend(snf1.getNetworkSideControllers())
    mem_cntrls.extend(snf1.getAllControllers())

    all_rnf1 = rnf1.getAllControllers()
    mn1 = CHI_MN(ruby_system, all_rnf1)
    all_cntrls.extend(mn1.getAllControllers())
    network_nodes.append(mn1)
    network_cntrls.extend(mn1.getNetworkSideControllers())

    c2cg1 = CHI_C2CG(ruby_system, full_range)
    all_cntrls.extend(c2cg1.getAllControllers())
    network_nodes.append(c2cg1)
    network_cntrls.extend(c2cg1.getNetworkSideControllers())

    # --- Wire C2C link ---
    bridges = wireC2CLink(c2cg0, c2cg1, ruby_system)
    if bridges is not None:
        system.c2c_bridge_a2b = bridges[0]
        system.c2c_bridge_b2a = bridges[1]

    # --- Downstream routing ---
    hnf0_cntrls = hnf0.getAllControllers()
    hnf1_cntrls = hnf1.getAllControllers()
    c2cg0_cntrls = c2cg0.getAllControllers()
    c2cg1_cntrls = c2cg1.getAllControllers()
    snf0_cntrls = snf0.getAllControllers()
    snf1_cntrls = snf1.getAllControllers()

    # RNFs route to local HNF + local C2CG
    rnf0.setDownstream(hnf0_cntrls + c2cg0_cntrls)
    rnf1.setDownstream(hnf1_cntrls + c2cg1_cntrls)

    # HNFs route to local SNF
    hnf0.setDownstream(snf0_cntrls)
    hnf1.setDownstream(snf1_cntrls)

    # Store on ruby_system for test access
    ruby_system.rnf = [rnf0, rnf1]
    ruby_system.hnf = [hnf0, hnf1]
    ruby_system.snf = [snf0, snf1]
    ruby_system.mn = [mn0, mn1]
    ruby_system.c2cg = [c2cg0, c2cg1]

    # Data message size
    for cntrl in all_cntrls:
        cntrl.data_channel_size = params.data_width

    # Network config
    ruby_system.network.number_of_virtual_networks = 4
    ruby_system.network.control_msg_size = params.cntrl_msg_size
    ruby_system.network.data_msg_size = params.data_width
    if hasattr(options, "network") and options.network == "simple":
        ruby_system.network.buffer_size = params.router_buffer_size

    for k in dir(params):
        if not k.startswith("__"):
            setattr(options, k, getattr(params, k))

    return (
        network_nodes,
        network_cntrls,
        all_cntrls,
        cpu_sequencers,
        mem_cntrls,
        (c2cg0, c2cg1),
    )
