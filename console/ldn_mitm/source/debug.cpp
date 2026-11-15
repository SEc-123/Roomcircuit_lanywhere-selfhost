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

const size_t TlsBackupSize = 0x100;

static std::atomic_bool g_logging_enabled = false;
#define BACKUP_TLS() u8 _tls_backup[TlsBackupSize];memcpy(_tls_backup, armGetTls(), TlsBackupSize);
#define RESTORE_TLS() memcpy(armGetTls(), _tls_backup, TlsBackupSize);

#define MIN(a, b) (((a) > (b)) ? (b) : (a))

ams::Result SetLogging(u32 enabled)
{
    g_logging_enabled = enabled != 0;
    if (g_logging_enabled) {
        const ams::Result result = ams::log::Initialize();
        if (R_FAILED(result)) g_logging_enabled = false;
        return result;
    }
    return 0;
}

ams::Result GetLogging(u32 *enabled)
{
    *enabled = g_logging_enabled;
    return 0;
}

namespace ams::log {
    namespace {
        constexpr const char LogFilePath[] = "sdmc:/ldn_mitm.log";
        constexpr s64 MaxLogBytes = 256 * 1024;
        os::Mutex g_file_log_lock(true);
    }

    Result Initialize() {
        if (!g_logging_enabled) return 0;
        std::scoped_lock lock(g_file_log_lock);
        bool exists = false;
        R_TRY(fs::HasFile(&exists, LogFilePath));
        if (!exists) R_TRY(fs::CreateFile(LogFilePath, 0));
        fs::FileHandle file;
        R_TRY(fs::OpenFile(&file, LogFilePath, fs::OpenMode_Write | fs::OpenMode_AllowAppend));
        fs::CloseFile(file);
        return 0;
    }

    void Finalize() { g_logging_enabled = false; }

    void LogStr(const char *format, std::va_list arguments) {
        if (!g_logging_enabled) return;
        std::scoped_lock lock(g_file_log_lock);
        BACKUP_TLS();
        ON_SCOPE_EXIT { RESTORE_TLS(); };
        fs::FileHandle file;
        if (R_FAILED(fs::OpenFile(&file, LogFilePath, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) {
            g_logging_enabled = false;
            return;
        }
        ON_SCOPE_EXIT { fs::CloseFile(file); };
        s64 offset = 0;
        if (R_FAILED(fs::GetFileSize(&offset, file)) || offset < 0) { g_logging_enabled = false; return; }
        char text[0x200];
        const auto *thread = os::GetCurrentThread();
        const auto time = os::GetSystemTick().ToTimeSpan();
        const int prefix = util::TSNPrintf(text, sizeof(text), "[ts: %6lums t: %lu] ",
            time.GetMilliSeconds(), os::GetThreadId(thread));
        const size_t used = std::min<size_t>(prefix > 0 ? prefix : 0, sizeof(text) - 1);
        const int body = util::TVSNPrintf(text + used, sizeof(text) - used, format, arguments);
        const size_t count = used + std::min<size_t>(body > 0 ? body : 0, sizeof(text) - used - 1);
        if (offset > MaxLogBytes - static_cast<s64>(count)) {
            if (R_FAILED(fs::SetFileSize(file, 0))) { g_logging_enabled = false; return; }
            offset = 0;
        }
        if (R_FAILED(fs::WriteFile(file, offset, text, count, fs::WriteOption::Flush)))
            g_logging_enabled = false;
    }

    void LogHexImpl(const void *data, int size) {
        if (!g_logging_enabled || size <= 0 || !data) return;
        std::scoped_lock lock(g_file_log_lock);
        const auto *bytes = static_cast<const u8 *>(data);
        LogFormatImpl("Bin Log: %d (%p)\n", size, data);
        for (int offset = 0; g_logging_enabled && offset < size; offset += 16) {
            char line[33]{};
            const int count = std::min(size - offset, 16);
            for (int i = 0; i < count; i++) std::snprintf(line + i * 2, 3, "%02x", bytes[offset + i]);
            LogFormatImpl("%s\n", line);
        }
    }

    void LogFormatImpl(const char *format, ...) {
        std::va_list arguments;
        va_start(arguments, format);
        LogStr(format, arguments);
        va_end(arguments);
    }
}
