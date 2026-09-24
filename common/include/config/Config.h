#ifndef FORGESCHED_CONFIG_H
#define FORGESCHED_CONFIG_H

#include <string>
#include <unordered_map>

namespace ForgeSched {

class Config {
public:
    static Config& instance();

    bool load(const std::string& filename);

    std::string getString(const std::string& key, const std::string& default_value) const;
    int getInt(const std::string& key, int default_value) const;
    bool getBool(const std::string& key, bool default_value) const;
    bool contains(const std::string& key) const;

        Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

private:
    Config() = default;
    ~Config() = default;

    std::string trim(const std::string& str) const;

    std::unordered_map<std::string, std::string> config_map_;
};

} // namespace ForgeSched

#endif // FORGESCHED_CONFIG_H