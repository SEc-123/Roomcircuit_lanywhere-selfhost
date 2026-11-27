/*
 * Copyright (c) 2018 Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "debug.hpp"
#include <cinttypes>

const size_t TlsBackupSize = 0x100;

#define BACKUP_TLS() u8 _tls_backup[TlsBackupSize];memcpy(_tls_backup, armGetTls(), TlsBackupSize);
#define RESTORE_TLS() memcpy(armGetTls(), _tls_backup, TlsBackupSize);

#define MIN(a, b) (((a) > (b)) ? (b) : (a))

namespace ams::log {
    namespace {
        constexpr const char LogFilePath[] = "sdmc:/ldn_mitm.log";
        constexpr s64 MaxLogBytes = 256 * 1024;
        os::Mutex g_file_log_lock(true);
        static_assert(std::atomic<u64>::is_always_lock_free);
        std::atomic<u64> g_session{};
        u64 g_last_session{}; // Protected by g_file_log_lock.

        Result AppendLocked(const char *text, size_t count) {
            fs::FileHandle file;
            R_TRY(fs::OpenFile(&file, LogFilePath, fs::OpenMode_Write | fs::OpenMode_AllowAppend));
            ON_SCOPE_EXIT { fs::CloseFile(file); };
            s64 offset{};
            R_TRY(fs::GetFileSize(&offset, file));
            if (offset < 0) return MAKERESULT(0xCB, 32);
            if (offset > MaxLogBytes - static_cast<s64>(count)) {
                R_TRY(fs::SetFileSize(file, 0));
                offset = 0;
            }
            return fs::WriteFile(file, offset, text, count, fs::WriteOption::Flush);
        }

        Result SetEnabled(bool enabled) {
            std::scoped_lock lock(g_file_log_lock);
            if (!enabled) { g_session.store(0); return 0; }
            if (g_session.load()) return 0;
            BACKUP_TLS();
            ON_SCOPE_EXIT { RESTORE_TLS(); };
            bool exists{};
            R_TRY(fs::HasFile(&exists, LogFilePath));
            if (!exists) R_TRY(fs::CreateFile(LogFilePath, 0));
            const u64 next = g_last_session == UINT64_MAX ? 1 : g_last_session + 1;
            char line[144];
            const int n = util::TSNPrintf(line, sizeof(line),
                "[ts: %" PRId64 "ms] ldn.logging version=%s session=%" PRIu64 " enabled=1\n",
                static_cast<s64>(os::GetSystemTick().ToTimeSpan().GetMilliSeconds()), GITDESCVER, next);
            if (n <= 0 || static_cast<size_t>(n) >= sizeof(line)) return MAKERESULT(0xCB, 32);
            R_TRY(AppendLocked(line, static_cast<size_t>(n)));
            g_last_session = next;
            g_session.store(next);
            return 0;
        }
    }

    u64 LoggingSession() { return g_session.load(); }

    void Finalize() { SetEnabled(false); }

    void LogStr(u64 session, const char *format, std::va_list arguments) {
        if (!session) return;
        std::scoped_lock lock(g_file_log_lock);
        if (LoggingSession() != session) return;
        BACKUP_TLS();
        ON_SCOPE_EXIT { RESTORE_TLS(); };
        char text[0x200];
        const auto *thread = os::GetCurrentThread();
        const auto time = os::GetSystemTick().ToTimeSpan();
        const int prefix = util::TSNPrintf(text, sizeof(text), "[ts: %6lums t: %lu] ",
            time.GetMilliSeconds(), os::GetThreadId(thread));
        const size_t used = std::min<size_t>(prefix > 0 ? prefix : 0, sizeof(text) - 1);
        const int body = util::TVSNPrintf(text + used, sizeof(text) - used, format, arguments);
        const size_t count = used + std::min<size_t>(body > 0 ? body : 0, sizeof(text) - used - 1);
        if (R_FAILED(AppendLocked(text, count))) g_session.store(0);
    }

    void LogHexImpl(const void *data, int size) {
        if (!LoggingSession() || size <= 0 || !data) return;
        std::scoped_lock lock(g_file_log_lock);
        const auto *bytes = static_cast<const u8 *>(data);
        LogFormatImpl("Bin Log: %d (%p)\n", size, data);
        for (int offset = 0; LoggingSession() && offset < size; offset += 16) {
            char line[33]{};
            const int count = std::min(size - offset, 16);
            for (int i = 0; i < count; i++) std::snprintf(line + i * 2, 3, "%02x", bytes[offset + i]);
            LogFormatImpl("%s\n", line);
        }
    }

    void LogFormatImpl(const char *format, ...) {
        std::va_list arguments;
        va_start(arguments, format);
        LogStr(LoggingSession(), format, arguments);
        va_end(arguments);
    }

    void LogFormatForSession(u64 session, const char *format, ...) {
        std::va_list arguments;
        va_start(arguments, format);
        LogStr(session, format, arguments);
        va_end(arguments);
    }
}

ams::Result SetLogging(u32 enabled) { return ams::log::SetEnabled(enabled != 0); }

ams::Result GetLogging(u32 *enabled) {
    *enabled = ams::log::LoggingSession() != 0;
    return 0;
}
