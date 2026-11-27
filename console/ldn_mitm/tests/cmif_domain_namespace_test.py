#!/usr/bin/env python3
"""Exercise the vendored CMIF domain pool with independent forwarded domain IDs."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3] / "console/Atmosphere-libs/libstratosphere"
SHIM = r"""
#include <array>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

using u32 = uint32_t;
using Result = u32;
#define NON_COPYABLE(T) T(const T &) = delete; T &operator=(const T &) = delete;
#define NON_MOVEABLE(T) T(T &&) = delete; T &operator=(T &&) = delete;
#define AMS_ABORT_UNLESS(condition) do { if (!(condition)) { std::fprintf(stderr, "CMIF assertion: %s\n", #condition); std::exit(1); } } while (false)
#define R_UNLESS(condition, result) if (!(condition)) return (result)
#define R_TRY(expression) do { const Result rc = (expression); if (rc) return rc; } while (false)
#define R_SUCCEED() return 0

namespace ams {
namespace os { using SdkMutex = std::mutex; }
namespace util {
    template <typename T> struct alignas(T) TypedStorage { unsigned char value[sizeof(T)]; };
    struct IntrusiveListNode {};
    template <typename T> class TestList {
        std::list<T *> values;
        public:
            struct Iterator {
                typename std::list<T *>::iterator position;
                T &operator*() const { return **position; }
                Iterator &operator++() { ++position; return *this; }
                bool operator==(const Iterator &) const = default;
            };
            Iterator begin() { return {values.begin()}; }
            Iterator end() { return {values.end()}; }
            bool empty() const { return values.empty(); }
            T &front() { return *values.front(); }
            void push_front(T &item) { values.push_front(&item); }
            void push_back(T &item) { values.push_back(&item); }
            void pop_front() { values.pop_front(); }
            Iterator iterator_to(T &item) {
                for (auto it = begin(); it != end(); ++it) if (&*it == &item) return it;
                throw std::runtime_error("entry not linked");
            }
            void erase(Iterator it) { values.erase(it.position); }
    };
    template <auto Member> struct IntrusiveListMemberTraits;
    template <typename T, IntrusiveListNode T::*Member> struct IntrusiveListMemberTraits<Member> {
        using ListType = TestList<T>;
    };
}
namespace sf {
namespace impl {
    struct ServiceObjectImplBase2 {
        std::atomic<int> references{1};
        void AddReferenceImpl() { references.fetch_add(1); }
        bool ReleaseImpl() { return references.fetch_sub(1) == 1; }
    };
}
namespace cmif {
    struct DomainObjectId { u32 value{}; bool operator==(const DomainObjectId &) const = default; };
    constexpr DomainObjectId InvalidDomainObjectId{};
    constexpr Result ResultOutOfDomainEntries() { return 501; }
    struct ServiceObjectHolder {
        std::shared_ptr<int> object;
        explicit operator bool() const { return bool(object); }
        void Reset() { object.reset(); }
        ServiceObjectHolder Clone() { return {object}; }
    };
    struct ServerDomainBase {
        virtual Result ReserveIds(DomainObjectId *, size_t) = 0;
        virtual void ReserveSpecificIds(const DomainObjectId *, size_t) = 0;
        virtual void UnreserveIds(const DomainObjectId *, size_t) = 0;
        virtual void RegisterObject(DomainObjectId, ServiceObjectHolder &&) = 0;
        virtual ServiceObjectHolder UnregisterObject(DomainObjectId) = 0;
        virtual ServiceObjectHolder GetObject(DomainObjectId) = 0;
    };
    struct DomainServiceObject : ServerDomainBase {
        virtual void AddReference() = 0;
        virtual void Release() = 0;
        virtual ServerDomainBase *GetServerDomain() = 0;
    };
}
}
}
"""

CASES = r"""
using namespace ams::sf::cmif;

template <size_t Capacity> struct PoolStorage {
    std::array<ServerDomainManager::DomainEntryStorage, Capacity> entries;
};
template <size_t Capacity> class Manager : private PoolStorage<Capacity>, public ServerDomainManager {
    public:
        Manager() : ServerDomainManager(this->entries.data(), Capacity) {}
        DomainServiceObject *Open() { return AllocateDomainServiceObject(); }
    private:
        void *AllocateDomain() override { return new DomainStorage; }
        void FreeDomain(void *domain) override { delete reinterpret_cast<DomainStorage *>(domain); }
};

static void Add(DomainServiceObject &domain, DomainObjectId id, int value) {
    domain.ReserveSpecificIds(&id, 1);
    domain.RegisterObject(id, {std::make_shared<int>(value)});
}

int main() {
    try {
        Manager<8> manager;
        auto *ldn = manager.Open();
        auto *bsd = manager.Open();
        Add(*ldn, {1}, 101);
        Add(*bsd, {1}, 102);
        Add(*ldn, {2}, 201);
        Add(*bsd, {2}, 202);
        assert(*ldn->GetObject({1}).object == 101 && *bsd->GetObject({1}).object == 102);
        assert(*ldn->GetObject({2}).object == 201 && *bsd->GetObject({2}).object == 202);

        /* Clones share one domain. A held object must survive removal by a clone. */
        ldn->AddReference();
        auto ldn_copy = ldn->GetObject({2});
        assert(*ldn->UnregisterObject({2}).object == 201);
        assert(!ldn->GetObject({2}) && *bsd->GetObject({2}).object == 202);
        ldn->Release();
        assert(*ldn_copy.object == 201 && *ldn->GetObject({1}).object == 101);

        auto *ordinary = manager.Open();
        DomainObjectId generated[6]{};
        assert(ordinary->ReserveIds(generated, 6) == ResultOutOfDomainEntries());
        assert(ordinary->ReserveIds(generated, 5) == 0);
        ordinary->RegisterObject(generated[0], {std::make_shared<int>(303)});
        assert(*ordinary->GetObject(generated[0]).object == 303);
        ordinary->UnreserveIds(generated + 1, 2);
        ordinary->Release(); // Also frees two unused reserved entries.
        ldn->Release();
        bsd->Release();
        assert(*ldn_copy.object == 201);

        /* Two live threads can legally mirror equal IDs in distinct domains. */
        std::array<std::thread, 2> clients;
        for (size_t i = 0; i < clients.size(); ++i) {
            clients[i] = std::thread([&, i] {
                for (int turn = 0; turn < 49; ++turn) {
                    auto *domain = manager.Open();
                    Add(*domain, {1}, int(i));
                    Add(*domain, {0x10001}, turn);
                    auto clone = domain->GetObject({0x10001});
                    assert(*domain->GetObject({1}).object == int(i));
                    domain->Release();
                    assert(*clone.object == turn);
                }
            });
        }
        for (auto &client : clients) client.join();
        std::puts("PASS mirrored IDs, clone lifetime, rollback, concurrent namespaces and pool reuse");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "CMIF domain pool rejected independent domains: %s\n", error.what());
        return 1;
    }
}
"""

header = (ROOT / "include/stratosphere/sf/cmif/sf_cmif_domain_manager.hpp").read_text()
source = (ROOT / "source/sf/cmif/sf_cmif_domain_manager.cpp").read_text()
# Keep the real manager declaration and implementation. Supply only the host
# storage, reference holder and list primitives the implementation uses.
scope = "namespace ams::sf::cmif {"
with tempfile.TemporaryDirectory(prefix="rc-cmif-domains-") as directory:
    test = Path(directory) / "test.cpp"
    executable = test.with_suffix("")
    test.write_text(SHIM + header[header.index(scope):] + source[source.index(scope):] + CASES)
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    subprocess.run(compiler + ["-std=c++20", "-Wall", "-Wextra", "-Werror",
                               "-pthread", str(test), "-o", str(executable)], check=True, timeout=50)
    subprocess.run([str(executable)], check=True, timeout=10)
