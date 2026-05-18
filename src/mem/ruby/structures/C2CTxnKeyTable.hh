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

#ifndef __MEM_RUBY_STRUCTURES_C2CTXNKEYTABLE_HH__
#define __MEM_RUBY_STRUCTURES_C2CTXNKEYTABLE_HH__

#include <algorithm>
#include <cassert>
#include <unordered_map>
#include <vector>

#include "base/stats/group.hh"
#include "base/types.hh"
#include "mem/ruby/common/MachineID.hh"

namespace gem5
{
namespace ruby
{

class C2CTxnKeyTable
{
  private:
    struct ScopedTxnKey
    {
        MachineID endpoint;
        Addr txnId;
        Addr addr;

        bool
        operator==(const ScopedTxnKey &other) const
        {
            return endpoint == other.endpoint && txnId == other.txnId &&
                   addr == other.addr;
        }
    };

    struct ScopedTxnKeyHash
    {
        std::size_t
        operator()(const ScopedTxnKey &key) const
        {
            return std::hash<MachineID>()(key.endpoint) ^
                   (std::hash<Addr>()(key.txnId) << 1) ^
                   (std::hash<Addr>()(key.addr) << 2);
        }
    };

  public:
    C2CTxnKeyTable(statistics::Group *parent) {}

    void
    bindC2COut(Addr txn_id, Addr key)
    {
        m_c2cOutKeys[txn_id] = key;
    }

    bool
    hasC2COut(Addr txn_id) const
    {
        return contains(m_c2cOutKeys, txn_id);
    }

    Addr
    lookupC2COut(Addr txn_id) const
    {
        return lookup(m_c2cOutKeys, txn_id);
    }

    void
    bindC2CInScoped(MachineID peer, Addr txn_id, Addr addr, Addr key)
    {
        m_c2cInScopedKeys[ScopedTxnKey{peer, txn_id, addr}] = key;
    }

    bool
    hasC2CInScoped(MachineID peer, Addr txn_id, Addr addr) const
    {
        return contains(m_c2cInScopedKeys, ScopedTxnKey{peer, txn_id, addr});
    }

    Addr
    lookupC2CInScoped(MachineID peer, Addr txn_id, Addr addr) const
    {
        return lookup(m_c2cInScopedKeys, ScopedTxnKey{peer, txn_id, addr});
    }

    void
    bindLocal(MachineID src, Addr txn_id, Addr addr, Addr key)
    {
        auto &keys = m_localKeys[ScopedTxnKey{src, txn_id, addr}];
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            keys.push_back(key);
        }
    }

    bool
    hasLocal(MachineID src, Addr txn_id, Addr addr) const
    {
        auto it = m_localKeys.find(ScopedTxnKey{src, txn_id, addr});
        return it != m_localKeys.end() && !it->second.empty();
    }

    Addr
    lookupLocal(MachineID src, Addr txn_id, Addr addr) const
    {
        auto it = m_localKeys.find(ScopedTxnKey{src, txn_id, addr});
        assert(it != m_localKeys.end());
        assert(!it->second.empty());
        return it->second.front();
    }

    void
    bindLocalTxn(Addr txn_id, Addr key)
    {
        m_localTxnKeys[txn_id] = key;
    }

    bool
    hasLocalTxn(Addr txn_id) const
    {
        return contains(m_localTxnKeys, txn_id);
    }

    Addr
    lookupLocalTxn(Addr txn_id) const
    {
        return lookup(m_localTxnKeys, txn_id);
    }

    void
    bindAddress(Addr addr, Addr key)
    {
        auto &keys = m_addrKeys[addr];
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            keys.push_back(key);
        }
    }

    bool
    hasAddress(Addr addr) const
    {
        auto it = m_addrKeys.find(addr);
        return it != m_addrKeys.end() && !it->second.empty();
    }

    Addr
    lookupAddress(Addr addr) const
    {
        auto it = m_addrKeys.find(addr);
        assert(it != m_addrKeys.end());
        assert(!it->second.empty());
        return it->second.front();
    }

    void
    eraseKey(Addr key)
    {
        eraseByValue(m_c2cOutKeys, key);
        eraseByValue(m_c2cInScopedKeys, key);
        eraseVectorByValue(m_localKeys, key);
        eraseByValue(m_localTxnKeys, key);

        for (auto it = m_addrKeys.begin(); it != m_addrKeys.end();) {
            auto &keys = it->second;
            keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
            if (keys.empty()) {
                it = m_addrKeys.erase(it);
            } else {
                ++it;
            }
        }
    }

  private:
    template <class Map, class Key>
    bool
    contains(const Map &map, const Key &key) const
    {
        return map.find(key) != map.end();
    }

    template <class Map, class Key>
    Addr
    lookup(const Map &map, const Key &key) const
    {
        auto it = map.find(key);
        assert(it != map.end());
        return it->second;
    }

    template <class Key, class Hash = std::hash<Key>>
    void
    eraseByValue(std::unordered_map<Key, Addr, Hash> &map, Addr value)
    {
        for (auto it = map.begin(); it != map.end();) {
            if (it->second == value) {
                it = map.erase(it);
            } else {
                ++it;
            }
        }
    }

    template <class Key, class Hash = std::hash<Key>>
    void
    eraseVectorByValue(std::unordered_map<Key, std::vector<Addr>, Hash> &map,
                       Addr value)
    {
        for (auto it = map.begin(); it != map.end();) {
            auto &keys = it->second;
            keys.erase(std::remove(keys.begin(), keys.end(), value),
                       keys.end());
            if (keys.empty()) {
                it = map.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::unordered_map<Addr, Addr> m_c2cOutKeys;
    std::unordered_map<ScopedTxnKey, Addr, ScopedTxnKeyHash> m_c2cInScopedKeys;
    std::unordered_map<Addr, Addr> m_localTxnKeys;
    std::unordered_map<ScopedTxnKey, std::vector<Addr>, ScopedTxnKeyHash>
        m_localKeys;
    std::unordered_map<Addr, std::vector<Addr>> m_addrKeys;
};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_STRUCTURES_C2CTXNKEYTABLE_HH__
