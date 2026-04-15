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
CHI C2C Gateway configuration classes.

Extends the CHI_config classes with a C2CG (Chip-to-Chip Gateway) node
for multi-chip coherency simulation.
"""

from CHI_config import (
    CHI_Node,
    TriggerMessageBuffer,
    Versions,
)

from m5.objects import *


class CHI_C2CController(CHI_CHI_C2C_Controller):
    """
    Default parameters for a C2C Gateway controller.
    """

    def __init__(self, ruby_system, addr_ranges):
        super().__init__(
            version=Versions.getVersion(CHI_CHI_C2C_Controller),
            ruby_system=ruby_system,
            triggerQueue=TriggerMessageBuffer(),
            retryTriggerQueue=TriggerMessageBuffer(),
            reqRdy=TriggerMessageBuffer(),
        )
        self.transitions_per_cycle = 1024
        self.addr_ranges = addr_ranges
        self.number_of_TBEs = 32
        self.number_of_snoop_TBEs = 16
        self.number_of_dvm_TBEs = 4

        # Feature support (Phase 5: interface management)
        self.timed_init = False
        self.dvm_support = True
        self.snoop_support = True
        self.atomic_support = True


class CHI_C2CG(CHI_Node):
    """
    Encapsulates a CHI C2C Gateway node.

    Each C2CG acts as a bridge between the local chip's Ruby network
    and the C2C link to the remote chip. It handles TxnID translation,
    request/response forwarding, and snoop bridging.
    """

    def __init__(self, ruby_system, addr_ranges, parent=None):
        super().__init__(ruby_system)

        self._cntrl = CHI_C2CController(ruby_system, addr_ranges)

        if parent is not None:
            parent.cntrl = self._cntrl
        else:
            self.cntrl = self._cntrl

        self.connectController(self._cntrl)

        # C2C-side buffers are created by wireC2CLink() when wiring
        # two C2CGs together. For standalone use, create default stubs.
        self._cntrl.c2cTxReq = MessageBuffer()
        self._cntrl.c2cTxSnp = MessageBuffer()
        self._cntrl.c2cTxRsp = MessageBuffer()
        self._cntrl.c2cTxDat = MessageBuffer()
        self._cntrl.c2cRxReq = MessageBuffer()
        self._cntrl.c2cRxSnp = MessageBuffer()
        self._cntrl.c2cRxRsp = MessageBuffer()
        self._cntrl.c2cRxDat = MessageBuffer()
        self._cntrl.c2cTxMisc = MessageBuffer()
        self._cntrl.c2cRxMisc = MessageBuffer()

    def getAllControllers(self):
        return [self._cntrl]

    def getNetworkSideControllers(self):
        return [self._cntrl]


def wireC2CLink(c2cg_a, c2cg_b):
    """
    Wire two C2CG nodes' TX/RX MessageBuffers together.

    In Phase 1, this is a simple pass-through: the TX buffer of one
    C2CG is the RX buffer of the other.
    """
    ca = c2cg_a.getAllControllers()[0]
    cb = c2cg_b.getAllControllers()[0]

    for ch in ["Req", "Snp", "Rsp", "Dat", "Misc"]:
        buf_a2b = MessageBuffer()
        buf_b2a = MessageBuffer()
        setattr(ca, f"c2cTx{ch}", buf_a2b)
        setattr(cb, f"c2cRx{ch}", buf_a2b)
        setattr(cb, f"c2cTx{ch}", buf_b2a)
        setattr(ca, f"c2cRx{ch}", buf_b2a)
