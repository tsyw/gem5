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

#ifndef __MEM_RUBY_STRUCTURES_C2CREADMERGETABLE_HH__
#define __MEM_RUBY_STRUCTURES_C2CREADMERGETABLE_HH__

#include <unordered_map>

#include "base/stats/group.hh"
#include "base/types.hh"
#include "mem/ruby/common/NetDest.hh"

namespace gem5
{
namespace ruby
{

class C2CReadMergeTable
{
  public:
    C2CReadMergeTable(statistics::Group *parent) {}

    void
    addSharers(Addr addr, const NetDest &sharers)
    {
        auto it = m_sharers.find(addr);
        if (it == m_sharers.end()) {
            m_sharers.emplace(addr, sharers);
        } else {
            it->second.addNetDest(sharers);
        }
    }

    NetDest
    expandSharers(Addr addr, const NetDest &requested) const
    {
        NetDest expanded = requested;
        auto it = m_sharers.find(addr);
        if (it != m_sharers.end() &&
            requested.intersectionIsNotEmpty(it->second)) {
            expanded.addNetDest(it->second);
        }
        return expanded;
    }

  private:
    std::unordered_map<Addr, NetDest> m_sharers;
};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_STRUCTURES_C2CREADMERGETABLE_HH__
