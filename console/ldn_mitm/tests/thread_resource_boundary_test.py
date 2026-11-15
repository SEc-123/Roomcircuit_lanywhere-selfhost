#!/usr/bin/env python3
"""Run pinned SDK thread-creation methods with injected kernel resource failures."""
import os
from pathlib import Path
import re
import resource
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
sdk = root / 'console/Atmosphere-libs/libstratosphere/source/os/impl'
result_headers = root / 'console/Atmosphere-libs/libvapours/include/vapours/results'


def function(text, signature):
    start = text.index(signature)
    level = 0
    for offset in range(text.index('{', start), len(text)):
        if text[offset] == '{':
            level += 1
        elif text[offset] == '}':
            level -= 1
            if level == 0:
                return text[start:offset + 1]
    raise ValueError(signature)


horizon = function((sdk / 'os_thread_manager_impl.os.horizon.cpp').read_text(),
                   'Result ThreadManagerHorizonImpl::CreateThread(')
manager = function((sdk / 'os_thread_manager.cpp').read_text(),
                   'Result ThreadManager::CreateThread(')
priority_header = (sdk / 'os_thread_manager_impl.os.horizon.hpp').read_text()
priority_offset = int(re.search(r'UserThreadPriorityOffset\s*=\s*(\d+)', priority_header)[1])
macros = (result_headers / 'results_common.hpp').read_text()


def macro(name):
    start = re.search(r'#define ' + re.escape(name) + r'(?=[(\s])', macros).start()
    lines = []
    for line in macros[start:].splitlines():
        lines.append(line)
        if not line.rstrip().endswith('\\'):
            return '\n'.join(lines)
    raise ValueError(name)


svc_header = (result_headers / 'svc_results.hpp').read_text()
error_names = ['OutOfResource', 'LimitReached', 'OutOfMemory', 'OutOfHandles', 'InvalidState', 'InvalidPriority']
descriptions = {
    name: int(re.search(r'R_DEFINE_ERROR_RESULT\(' + name + r',\s*(\d+)\)', svc_header)[1])
    for name in error_names
}
error_types = '\n'.join(
    f'struct Result{name} {{ static bool Includes(int value) {{ return value == (({number} << 9) | 1); }} }};'
    for name, number in descriptions.items()
)
# Restore just the newly handled catch branch to reproduce the audited SDK policy.
original = re.sub(r'\s*// RoomCircuit: callers can recover.*?R_CATCH\(svc::ResultLimitReached, '
                  r'svc::ResultOutOfMemory, svc::ResultOutOfHandles\) \{\s*'
                  r'R_THROW\(os::ResultOutOfResource\(\)\);\s*\}', '', horizon, flags=re.S)
assert original != horizon

fixture = r'''
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
using Result = int;
using s32 = int;
using ThreadFunc = void (*)(void *);
using ThreadFunction = ThreadFunc;
static unsigned calls, sleeps, placed, deleted, started, waited;
static int injected, last_kernel_result;
static bool transient_failure;
static constexpr int KernelPriorityOffset = /* PRIORITY_OFFSET */;

int threadCreate(void *, ThreadFunc, void *, void *, size_t, int priority, int) {
    assert(priority == 10 + KernelPriorityOffset);
    calls++;
    last_kernel_result = transient_failure && calls > 2 ? 0 : injected;
    return last_kernel_result;
}
static void InvokeThread(uintptr_t) {}
static int ConvertToHorizonPriority(int priority) { return priority + KernelPriorityOffset; }
struct TimeSpan { static int FromMilliSeconds(int ms) { return ms; } };
namespace ams::svc {
/* ERROR_TYPES */
}
namespace ams::os {
struct ResultOutOfResource { operator int() const { return 9009; } };
void SleepThread(int ms) { assert(ms == 10); sleeps++; }
}
namespace ams::result::impl {
template<typename... T> bool EvaluateAnyResultIncludes(int value) { return (T::Includes(value) || ...); }
}
#define R_FAILED(result) ((result) != 0)
#define R_THROW(result) return int(result)
#define R_SUCCEED() return 0
#define AMS_UNLIKELY(value) (value)
#define AMS_CALL_ON_RESULT_ABORT_IMPL(text, value) std::abort()
#define AMS_ABORT_UNLESS(value) do { if (!(value)) std::abort(); } while (0)
#define R_TRY(result) do { const int checked = (result); if (checked) return checked; } while (0)
/* SDK_MACROS */
// Model only the SDK's scope-result boundary: its failure action runs after the
// extracted method returns an error, while all thread state transitions below
// remain in the actual ThreadManager::CreateThread body.
template<typename Action> struct FailureGuard {
    Action action;
    ~FailureGuard() { if (last_kernel_result) action(); }
};
struct FailureFactory {};
template<typename Action> auto operator+(FailureFactory, Action action) { return FailureGuard<Action>{action}; }
#define ON_RESULT_FAILURE auto failure_cleanup = FailureFactory{} + [&]()

namespace ams::os::impl {
struct ThreadType {
    enum { State_NotInitialized, State_Initialized } state = State_NotInitialized;
    void *thread_impl = nullptr;
    void *stack = nullptr;
    size_t stack_size = 16384;
    int base_priority = 10;
    bool aliased = false;
};
struct ThreadManagerHorizonImpl { Result CreateThread(ThreadType *, s32); };
struct ThreadManager {
    ThreadManagerHorizonImpl m_impl;
    Result CreateThread(ThreadType *, ThreadFunc, void *, void *, size_t, s32, s32);
    bool CreateAliasStackUnsafe(ThreadType *thread) { thread->aliased = true; return true; }
    void DeleteAliasStackUnsafe(ThreadType *thread) { assert(thread->aliased); thread->aliased = false; deleted++; }
    void PlaceThreadObjectUnderThreadManagerSafe(ThreadType *) { placed++; }
};
void SetupThreadObjectUnsafe(ThreadType *thread, void *, ThreadFunc, void *, void *, size_t, s32) {
    thread->state = ThreadType::State_Initialized;
}
/* HORIZON_METHOD */
/* MANAGER_METHOD */
}
int main(int argc, char **argv) {
    assert(argc == 3);
    injected = atoi(argv[1]);
    transient_failure = atoi(argv[2]) != 0;
    ams::os::impl::ThreadType thread;
    ams::os::impl::ThreadManager manager;
    const int result = manager.CreateThread(&thread, nullptr, nullptr, nullptr, 16384, 10, 0);
    if (result == 0) {
        started++;
        waited++;
        assert(thread.aliased && placed == 1 && deleted == 0);
    } else {
        assert(result == 9009 && !thread.aliased && placed == 0 && deleted == 1);
        assert(thread.state == ams::os::impl::ThreadType::State_NotInitialized);
    }
    printf("result=%d calls=%u sleeps=%u cleaned=%u started=%u waited=%u\n",
           result, calls, sleeps, deleted, started, waited);
}
'''
fixture = fixture.replace('/* ERROR_TYPES */', error_types)
fixture = fixture.replace('/* PRIORITY_OFFSET */', str(priority_offset))
fixture = fixture.replace('/* SDK_MACROS */', '\n'.join(macro(name) for name in [
    'R_CURRENT_RESULT', 'R_TRY_CATCH', 'R_CATCH', 'R_END_TRY_CATCH_WITH_ABORT_UNLESS', 'R_ABORT_UNLESS']))
fixture = fixture.replace('/* MANAGER_METHOD */', manager)


def no_core_dump():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


with tempfile.TemporaryDirectory(prefix='roomcircuit-thread-resource-') as temporary:
    temporary = Path(temporary)
    for label, body in [('original', original), ('fixed', horizon)]:
        source, executable = temporary / (label + '.cpp'), temporary / label
        source.write_text(fixture.replace('/* HORIZON_METHOD */', body))
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + [
            '-std=c++17', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(executable)], check=True)
        for name in ['Success', *error_names, 'TransientOutOfResource']:
            transient = name == 'TransientOutOfResource'
            value = 0 if name == 'Success' else (descriptions['OutOfResource' if transient else name] << 9) | 1
            run = subprocess.run([str(executable), str(value), '1' if transient else '0'],
                                 text=True, capture_output=True, preexec_fn=no_core_dump, timeout=5)
            graceful = name in ['Success', 'OutOfResource', 'TransientOutOfResource'] or (
                label == 'fixed' and name in ['LimitReached', 'OutOfMemory', 'OutOfHandles'])
            if not graceful:
                assert run.returncode == -6, (label, name, run.returncode, run.stdout, run.stderr)
                continue
            assert run.returncode == 0, (label, name, run.stdout, run.stderr)
            if name in ['Success', 'TransientOutOfResource']:
                assert 'result=0' in run.stdout and 'cleaned=0 started=1 waited=1' in run.stdout
                if transient:
                    assert 'calls=3 sleeps=2' in run.stdout
            else:
                assert 'result=9009' in run.stdout and 'cleaned=1 started=0 waited=0' in run.stdout
                assert ('calls=10 sleeps=9' if name == 'OutOfResource' else 'calls=1 sleeps=0') in run.stdout
print('PASS actual SDK thread methods: original quota/memory/handle errors abort; fixed resource errors return, '
      'unwind alias/state, and skip Start/Wait; success, transient retries and invalid-argument/state abort policy preserved')
