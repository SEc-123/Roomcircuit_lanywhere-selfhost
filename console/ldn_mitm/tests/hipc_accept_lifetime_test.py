#!/usr/bin/env python3
"""Run the vendored session factory, accept, registration and recover paths with a controlled kernel port."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from discovery_route_test import function

ROOT = Path(__file__).resolve().parents[3]
SDK = ROOT / 'console/Atmosphere-libs/libstratosphere'
header = (SDK / 'include/stratosphere/sf/hipc/sf_hipc_server_session_manager.hpp').read_text()
source = (SDK / 'source/sf/hipc/sf_hipc_server_session_manager.cpp').read_text()
manager = (SDK / 'source/sf/hipc/sf_hipc_server_manager.cpp').read_text()
factory = function(header, 'template<typename Constructor>')
methods = ('AcceptSessionImpl', 'AcceptMitmSessionImpl', 'RegisterSessionImpl', 'RegisterMitmSessionImpl')
implementation = '\n'.join(function(source, f'Result ServerSessionManager::{name}(') for name in methods)
recovery = function(manager, 'Result ServerManagerBase::RecoverAcceptFailure(')
fixture = r'''
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

using Result = int;
static Result returned;
#define R_TRY(expr) do { const Result rc = (expr); if (rc) return returned = rc; } while (0)
#define R_UNLESS(expr, err) do { if (!(expr)) return returned = (err); } while (0)
#define R_RETURN(expr) return returned = (expr)
#define R_SUCCEED() return returned = 0
#define R_SUCCEEDED(result) ((result) == 0)
#define AMS_ABORT_UNLESS(expr) assert(expr)
#define ATMOSPHERE_OS_HORIZON
template<typename Action> struct Cleanup {
    Action action;
    bool on_failure, active = true;
    void Cancel() { active = false; }
    ~Cleanup() { if (active && (!on_failure || returned)) action(); }
};
struct Scope { bool on_failure; };
template<typename Action> Cleanup<Action> operator+(Scope scope, Action action) {
    return {action, scope.on_failure};
}
#define SCOPE_GUARD Scope{false} + [&]()
#define ON_RESULT_FAILURE auto failure = Scope{true} + [&]()

template<int Value> struct Error {
    operator int() const { return Value; }
    static bool Includes(int result) { return result == Value; }
};
namespace sf::hipc {
    using ResultOutOfSessionMemory = Error<37>;
    using ResultPointerBufferTooSmall = Error<38>;
}
namespace cmif {
    struct ServiceObjectHolder {};
    struct PointerAndSize {
        std::size_t address{}, size{};
        std::size_t GetAddress() const { return address; }
        std::size_t GetSize() const { return size; }
    };
}
namespace os {
    using NativeHandle = int;
    int closed_handles;
    void CloseNativeHandle(NativeHandle h) { assert(h == 17); ++closed_handles; }
}
namespace svc {
    using ResultNotFound = Error<39>;
    Result forced_error = 0;
    bool pending;
    Result AcceptSession(int *out, int) {
        if (forced_error) return forced_error;
        if (!pending) return ResultNotFound();
        pending = false;
        *out = 17;
        return 0;
    }
}
struct Service { unsigned pointer_buffer_size = 0x400; };
struct ServerSession {
    cmif::PointerAndSize m_pointer_buffer, m_saved_message;
    static inline int live = 0;
    ServerSession(int h, cmif::ServiceObjectHolder &&) { assert(h == 17); ++live; }
    ServerSession(int h, cmif::ServiceObjectHolder &&o, std::shared_ptr<Service> &&)
        : ServerSession(h, std::move(o)) {}
    ~ServerSession() { --live; }
};
struct Server { int m_port_handle = 1; };
struct ServerManagerBase {
    enum class MitmAcceptStage { SlotReserved, KernelAccepted, RegisteringWait };
    static bool CanAnyManageMitmServers() { return true; }
    Result RecoverAcceptFailure(Server *server, Result result);
};
struct ServerSessionManager : ServerManagerBase {
    alignas(ServerSession) std::byte slot[sizeof(ServerSession)]{};
    bool reserved = false;
    int invalid_destructs = 0, destructs = 0, releases = 0;
    std::vector<MitmAcceptStage> milestones;
    ServerSession *AllocateSession() {
        if (reserved) return nullptr;
        reserved = true;
        return reinterpret_cast<ServerSession *>(slot);
    }
    void FreeSession(ServerSession *s) {
        assert(reserved && reinterpret_cast<void *>(s) == slot);
        reserved = false;
        ++releases;
    }
    void DestroySession(ServerSession *s) {
        // Detect an invalid destructor call without actually running one on raw storage.
        if (!ServerSession::live) ++invalid_destructs;
        else { std::destroy_at(s); ++destructs; }
        FreeSession(s);
    }
    void OnMitmAcceptStage(MitmAcceptStage stage) { milestones.push_back(stage); }
    cmif::PointerAndSize GetSessionPointerBuffer(ServerSession *) { return {0, 0x1000}; }
    cmif::PointerAndSize GetSessionSavedMessageBuffer(ServerSession *) { return {}; }
    void RegisterServerSessionToWait(ServerSession *) { assert(ServerSession::live == 1); }
    Result AcceptSessionImpl(ServerSession *, int, cmif::ServiceObjectHolder &&);
    Result AcceptMitmSessionImpl(ServerSession *, int, cmif::ServiceObjectHolder &&, std::shared_ptr<Service> &&);
    Result RegisterSessionImpl(ServerSession *, int, cmif::ServiceObjectHolder &&);
    Result RegisterMitmSessionImpl(ServerSession *, int, cmif::ServiceObjectHolder &&, std::shared_ptr<Service> &&);
    /* FACTORY */
};
/* METHODS */
/* RECOVERY */
int main() {
    for (bool mitm : {false, true}) {
        ServerSessionManager m;
        Server port;
        ServerSession *out = nullptr;
        auto forward = std::make_shared<Service>();
        auto accept = [&](ServerSession *s) {
            return mitm ? m.AcceptMitmSessionImpl(s, 1, {}, std::shared_ptr<Service>(forward)) :
                          m.AcceptSessionImpl(s, 1, {});
        };
        svc::pending = true;
        assert(m.CreateSessionImpl(&out, accept) == 0 && out && m.reserved);
        if (mitm) {
            using Stage = ServerManagerBase::MitmAcceptStage;
            assert((m.milestones == std::vector{Stage::SlotReserved, Stage::KernelAccepted, Stage::RegisteringWait}));
            assert(out->m_pointer_buffer.size == forward->pointer_buffer_size);
        }

        // A second client arrives with the only slot still occupied. Close exactly that client.
        svc::pending = true;
        const int closed = os::closed_handles;
        const auto full = m.CreateSessionImpl(&out, accept);
        assert(full == sf::hipc::ResultOutOfSessionMemory() && svc::pending);
        assert(m.RecoverAcceptFailure(&port, full) == 0 && !svc::pending);
        assert(m.RecoverAcceptFailure(&port, full) == 0);  // queue emptied meanwhile
        assert(os::closed_handles == closed + 1 && ServerSession::live == 1);
        svc::pending = true;
        svc::forced_error = 42;
        assert(m.RecoverAcceptFailure(&port, full) == 42 && svc::pending);
        m.DestroySession(out);
        out = nullptr;

        // The pool now has room but the kernel rejects accept before object construction.
        const auto failed = m.CreateSessionImpl(&out, accept);
        assert(failed == 42 && out == nullptr && !m.reserved && !ServerSession::live);
        assert(m.invalid_destructs == 0 && m.releases == 2 && m.destructs == 1);
        assert(m.RecoverAcceptFailure(&port, failed) == 42 && svc::pending);
        svc::forced_error = 0;
        svc::pending = false;
        const auto cancelled = m.CreateSessionImpl(&out, accept);
        assert(cancelled == svc::ResultNotFound() && m.RecoverAcceptFailure(&port, cancelled) == 0);

        if (mitm) {
            // The kernel has already dequeued this client; return its handle on size rejection.
            const int before = os::closed_handles;
            forward->pointer_buffer_size = 0x1001;
            svc::pending = true;
            const auto large = m.CreateSessionImpl(&out, accept);
            assert(large == sf::hipc::ResultPointerBufferTooSmall());
            assert(os::closed_handles == before + 1 && !m.reserved && out == nullptr);
            svc::pending = true;
            assert(m.RecoverAcceptFailure(&port, large) == 0 && svc::pending);
            forward->pointer_buffer_size = 0x1000;
        }

        // The next client still enters and leaves normally on exactly the same pool slot.
        svc::pending = true;
        assert(m.CreateSessionImpl(&out, accept) == 0 && !svc::pending);
        m.DestroySession(out);
        assert(!m.reserved && !ServerSession::live && m.destructs == 2 && m.invalid_destructs == 0);
    }
    std::puts("PASS normal/MITM failed accept, slot reuse, pending-client ownership, buffer rejection and next client");
}
'''
with tempfile.TemporaryDirectory(prefix='roomcircuit-hipc-accept-') as directory:
    directory = Path(directory)
    cpp = directory / 'accept.cpp'
    exe = directory / 'accept'
    cpp.write_text(fixture.replace('/* FACTORY */', factory).replace('/* METHODS */', implementation)
                   .replace('/* RECOVERY */', recovery))
    subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
        '-std=c++20', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
