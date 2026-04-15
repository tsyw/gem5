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

from m5.objects.ClockedObject import ClockedObject
from m5.params import *


class C2CPacketizerBridge(ClockedObject):
    """Unidirectional bridge that models Format X container bandwidth
    between two CHI C2C Gateway controllers.

    Drains TX MessageBuffers, packs messages into 12-granule containers
    (priority: RSP > DAT > SNP > REQ > MISC), and delivers cloned
    messages to RX MessageBuffers with configurable latency.
    """

    type = "C2CPacketizerBridge"
    cxx_header = "mem/ruby/protocol/chi/c2c/C2CPacketizerBridge.hh"
    cxx_class = "gem5::ruby::C2CPacketizerBridge"

    txReq = Param.MessageBuffer("TX REQ input buffer")
    txSnp = Param.MessageBuffer("TX SNP input buffer")
    txRsp = Param.MessageBuffer("TX RSP input buffer")
    txDat = Param.MessageBuffer("TX DAT input buffer")
    txMisc = Param.MessageBuffer("TX MISC input buffer")

    rxReq = Param.MessageBuffer("RX REQ output buffer")
    rxSnp = Param.MessageBuffer("RX SNP output buffer")
    rxRsp = Param.MessageBuffer("RX RSP output buffer")
    rxDat = Param.MessageBuffer("RX DAT output buffer")
    rxMisc = Param.MessageBuffer("RX MISC output buffer")

    container_latency = Param.Int(
        1, "Per-container latency across the C2C link (cycles)"
    )
    ruby_system = Param.RubySystem("Ruby system reference")
