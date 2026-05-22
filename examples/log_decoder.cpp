// log_decoder — offline audit CLI for logger-high shard files.
//
// Usage:
//   log_decoder <shard_file_or_dir> <server_priv.key> [--json]
//
//   shard_file_or_dir   either a single log-*.bin file or a directory containing them.
//   server_priv.key     raw 32-byte X25519 private key (matching the public key
//                       that was used at logging time).
//   --json              emit one JSON object per line instead of the default
//                       human-readable format.
//
// Exit codes:
//   0 success
//   1 invalid arguments / IO / crypto / parse error (message on stderr).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "audit/shard_reader.h"
#include "utils/log_level.h"

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kServerPrivBytes = 32;

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s <shard_file_or_dir> <server_priv.key> [--json]\n", argv0);
}

std::string ReadPrivKey(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in.good()) {
        throw std::runtime_error("cannot open private key file: " + p.string());
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    std::string raw = oss.str();
    if (raw.size() != kServerPrivBytes) {
        throw std::runtime_error(
            "server private key must be exactly 32 bytes (got " +
            std::to_string(raw.size()) + ")");
    }
    return raw;
}

std::string LevelName(std::uint32_t level_value) {
    return std::string(
        ::logger::ToString(static_cast<::logger::LogLevel>(level_value)));
}

// Format nanoseconds since system_clock epoch as ISO 8601 UTC with nanos:
// "2026-05-22T07:12:34.123456789Z".
std::string FormatTimeIso(std::int64_t time_ns) {
    using namespace std::chrono;
    const auto secs    = time_ns / 1'000'000'000LL;
    const auto sub_ns  = static_cast<long>(time_ns % 1'000'000'000LL);
    const std::time_t t = static_cast<std::time_t>(secs);

    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);

    char out[64];
    std::snprintf(out, sizeof(out), "%s.%09ldZ", buf, sub_ns);
    return out;
}

void JsonEscapeAppend(std::string& dst, std::string_view s) {
    dst.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  dst.append("\\\""); break;
            case '\\': dst.append("\\\\"); break;
            case '\b': dst.append("\\b");  break;
            case '\f': dst.append("\\f");  break;
            case '\n': dst.append("\\n");  break;
            case '\r': dst.append("\\r");  break;
            case '\t': dst.append("\\t");  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x",
                                  static_cast<unsigned int>(static_cast<unsigned char>(c)));
                    dst.append(esc);
                } else {
                    dst.push_back(c);
                }
        }
    }
    dst.push_back('"');
}

void PrintRecordText(const logger::proto::LogRecord& r) {
    std::string out;
    out.reserve(256);
    out += "[";
    out += FormatTimeIso(r.time_ns());
    out += "] [";
    out += LevelName(r.level());
    out += "] [";
    out += r.logger_name();
    out += "] (";
    out += std::to_string(r.pid());
    out += ":";
    out += std::to_string(r.tid());
    out += ") ";
    if (!r.file().empty()) {
        out += "(";
        out += r.file();
        out += ":";
        out += std::to_string(r.line());
        if (!r.func().empty()) {
            out += " ";
            out += r.func();
        }
        out += ") ";
    }
    out += r.message();
    out += "\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
}

void PrintRecordJson(const logger::proto::LogRecord& r) {
    std::string out;
    out.reserve(256);
    out += "{\"time\":";
    JsonEscapeAppend(out, FormatTimeIso(r.time_ns()));
    out += ",\"time_ns\":";
    out += std::to_string(r.time_ns());
    out += ",\"level\":";
    JsonEscapeAppend(out, LevelName(r.level()));
    out += ",\"logger\":";
    JsonEscapeAppend(out, r.logger_name());
    out += ",\"pid\":";
    out += std::to_string(r.pid());
    out += ",\"tid\":";
    out += std::to_string(r.tid());
    out += ",\"file\":";
    JsonEscapeAppend(out, r.file());
    out += ",\"line\":";
    out += std::to_string(r.line());
    out += ",\"func\":";
    JsonEscapeAppend(out, r.func());
    out += ",\"message\":";
    JsonEscapeAppend(out, r.message());
    out += "}\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        PrintUsage(argv[0]);
        return 1;
    }
    bool json_mode = false;
    if (argc == 4) {
        std::string_view flag = argv[3];
        if (flag == "--json") {
            json_mode = true;
        } else {
            std::fprintf(stderr, "unknown flag: %s\n", argv[3]);
            PrintUsage(argv[0]);
            return 1;
        }
    }

    try {
        const fs::path target = argv[1];
        const fs::path key_path = argv[2];

        if (!fs::exists(target)) {
            throw std::runtime_error("path does not exist: " + target.string());
        }

        const std::string priv = ReadPrivKey(key_path);

        std::vector<logger::proto::LogRecord> records;
        if (fs::is_directory(target)) {
            records = logger::audit::DecodeShardDir(target, priv);
        } else if (fs::is_regular_file(target)) {
            records = logger::audit::DecodeShardFile(target, priv);
        } else {
            throw std::runtime_error("target is neither file nor directory: " +
                                     target.string());
        }

        for (const auto& r : records) {
            if (json_mode) PrintRecordJson(r);
            else           PrintRecordText(r);
        }
        std::fflush(stdout);

        std::fprintf(stderr, "decoded %zu record(s)\n", records.size());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "log_decoder: %s\n", e.what());
        return 1;
    }
}
