#include "log/RunLogArchive.h"
#include <array>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <chrono>
#include <stdexcept>

namespace ForgeSched {
namespace {
constexpr uint64_t kFileLimit = 1024ULL * 1024 * 1024;
constexpr size_t kChunkLimit = 16384;
constexpr std::array<const char*, 8> names{{"stdout.log", "stderr.log", "result.env",
    "result.json", "flow_config", "evidence.run", "runner.log", "case-run.log"}};
int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::string decode(const std::string& value) {
    if (value.size() > 2 * kChunkLimit || value.size() % 2)
        throw std::invalid_argument("invalid log chunk length");
    std::string out(value.size() / 2, '\0');
    for (size_t i = 0; i < out.size(); ++i) {
        const int a = nibble(value[2 * i]), b = nibble(value[2 * i + 1]);
        if (a < 0 || b < 0) throw std::invalid_argument("invalid log hex");
        out[i] = static_cast<char>((a << 4) | b);
    }
    return out;
}
std::string encode(const std::string& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out(value.size() * 2, '\0');
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        out[2 * i] = digits[c >> 4];
        out[2 * i + 1] = digits[c & 15];
    }
    return out;
}
void writeAll(int fd, const std::string& bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("log write failed");
        offset += static_cast<size_t>(count);
    }
    if (::fsync(fd) != 0) throw std::runtime_error("log sync failed");
}
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
};
void syncDirectory(const std::filesystem::path& path) {
    Fd directory(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.value < 0 || ::fsync(directory.value) != 0)
        throw std::runtime_error("cannot sync log directory");
}
}
RunLogArchive::RunLogArchive(std::filesystem::path root) : root_(std::move(root)) {
    if (root_.empty()) throw std::invalid_argument("log archive root is empty");
    std::filesystem::create_directories(root_);
    if (std::filesystem::is_symlink(root_) || !std::filesystem::is_directory(root_))
        throw std::runtime_error("unsafe log archive root");
}
bool RunLogArchive::validRunId(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    for (char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}
bool RunLogArchive::validName(const std::string& value) {
    for (const char* name : names) if (value == name) return true;
    return false;
}
std::filesystem::path RunLogArchive::file(uint64_t task_id, const std::string& run_id,
                                           const std::string& name) const {
    if (!task_id || !validRunId(run_id) || !validName(name))
        throw std::invalid_argument("invalid log identity");
    auto parent = root_ / std::to_string(task_id);
    auto directory = parent / run_id;
    if (std::filesystem::is_symlink(parent) || std::filesystem::is_symlink(directory))
        throw std::runtime_error("unsafe log directory");
    return directory / name;
}
nlohmann::json RunLogArchive::append(uint64_t task_id, const std::string& run_id,
    const std::string& name, uint64_t offset, const std::string& hex, bool complete) {
    const auto bytes = decode(hex);
    if (offset > kFileLimit || bytes.size() > kFileLimit - offset)
        throw std::invalid_argument("log file limit exceeded");
    std::lock_guard<std::mutex> lock(mutex_);
    auto path = file(task_id, run_id, name);
    const auto task_dir = path.parent_path().parent_path();
    const bool new_task_dir = !std::filesystem::exists(task_dir);
    const bool new_run_dir = !std::filesystem::exists(path.parent_path());
    std::filesystem::create_directories(path.parent_path());
    if (new_task_dir) syncDirectory(root_);
    if (new_run_dir) syncDirectory(task_dir);
    if (std::filesystem::is_symlink(path) || std::filesystem::is_symlink(path.string() + ".done"))
        throw std::runtime_error("unsafe log file");
    const bool new_file = !std::filesystem::exists(path);
    Fd fd(::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (fd.value < 0) throw std::runtime_error("cannot open log file");
    const uint64_t size = std::filesystem::file_size(path);
    const bool done = std::filesystem::exists(path.string() + ".done");
    if (size > kFileLimit) throw std::runtime_error("log file limit exceeded");
    if (offset != size || done) return {{"next_offset", size}, {"complete", done}};
    writeAll(fd.value, bytes);
    if (new_file) syncDirectory(path.parent_path());
    const uint64_t next = size + bytes.size();
    if (complete) {
        Fd marker(::open((path.string() + ".done").c_str(),
                         O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (marker.value < 0) throw std::runtime_error("cannot complete log file");
        if (::fsync(marker.value) != 0) throw std::runtime_error("cannot sync log marker");
        syncDirectory(path.parent_path());
    }
    return {{"next_offset", next}, {"complete", complete}};
}
nlohmann::json RunLogArchive::list(uint64_t task_id, const std::string& run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json items = nlohmann::json::array();
    for (const char* name : names) {
        const auto path = file(task_id, run_id, name);
        if (std::filesystem::is_symlink(path)) continue;
        if (std::filesystem::is_regular_file(path))
            items.push_back({{"name", name}, {"size", std::filesystem::file_size(path)},
                {"complete", std::filesystem::is_regular_file(path.string() + ".done")}});
    }
    return {{"task_id", task_id}, {"run_id", run_id}, {"items", items}};
}
nlohmann::json RunLogArchive::read(uint64_t task_id, const std::string& run_id,
    const std::string& name, uint64_t offset, size_t limit) {
    if (!limit || limit > kChunkLimit) throw std::invalid_argument("invalid log read limit");
    std::lock_guard<std::mutex> lock(mutex_);
    const auto path = file(task_id, run_id, name);
    Fd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.value < 0) throw std::invalid_argument("log file not found");
    const auto size = std::filesystem::file_size(path);
    if (offset > size) throw std::invalid_argument("log offset beyond end");
    std::string bytes(std::min<uint64_t>(limit, size - offset), '\0');
    const auto count = ::pread(fd.value, bytes.data(), bytes.size(), static_cast<off_t>(offset));
    if (count < 0) throw std::runtime_error("log read failed");
    bytes.resize(static_cast<size_t>(count));
    return {{"task_id", task_id}, {"run_id", run_id}, {"name", name},
            {"offset", offset}, {"next_offset", offset + bytes.size()},
            {"size", size}, {"complete", std::filesystem::exists(path.string() + ".done")},
            {"data_hex", encode(bytes)}};
}
size_t RunLogArchive::pruneOlderThan(int days) {
    if (days < 1 || days > 3650) throw std::invalid_argument("invalid log retention days");
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * days);
    std::lock_guard<std::mutex> lock(mutex_);
    size_t removed = 0;
    for (const auto& task : std::filesystem::directory_iterator(root_)) {
        if (task.is_symlink() || !task.is_directory()) continue;
        const auto id = task.path().filename().string();
        if (id.empty() || id[0] == '0' || id.find_first_not_of("0123456789") != std::string::npos) continue;
        for (const auto& run : std::filesystem::directory_iterator(task.path())) {
            if (run.is_symlink() || !run.is_directory() || !validRunId(run.path().filename().string())) continue;
            bool safe = true, has_file = false;
            auto newest = std::filesystem::file_time_type::min();
            for (const auto& entry : std::filesystem::directory_iterator(run.path())) {
                const auto filename = entry.path().filename().string();
                const auto base = filename.size() > 5 && filename.substr(filename.size() - 5) == ".done"
                    ? filename.substr(0, filename.size() - 5) : filename;
                if (entry.is_symlink() || !entry.is_regular_file() || !validName(base)) { safe = false; break; }
                has_file = true;
                newest = std::max(newest, entry.last_write_time());
            }
            if (!safe || !has_file || newest >= cutoff) continue;
            for (const auto& entry : std::filesystem::directory_iterator(run.path()))
                std::filesystem::remove(entry.path());
            std::filesystem::remove(run.path());
            ++removed;
        }
        if (std::filesystem::is_empty(task.path())) std::filesystem::remove(task.path());
    }
    return removed;
}
} // namespace ForgeSched
