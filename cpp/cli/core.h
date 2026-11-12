// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>
#include <QStringList>
#include <filesystem>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <vector>
namespace ctl {
using J = nlohmann::json;
namespace fs = std::filesystem;
struct Error : std::runtime_error {
  int code;
  J details;
  Error(int c, std::string message, J d = nullptr)
      : std::runtime_error(std::move(message)), code(c), details(std::move(d)) {
  }
};
inline std::string str(QString const &s) { return s.toUtf8().toStdString(); }
inline QString qs(std::string const &s) { return QString::fromUtf8(s); }
inline std::string arg(J const &v) { return v.get<std::string>(); }
struct Input {
  std::string command;
  std::vector<std::string> positions;
  std::map<std::string, std::vector<std::string>> options;
  bool has(std::string const &k) const { return options.contains(k); }
  std::string get(std::string const &k, std::string fallback = {}) const;
  std::vector<std::string> many(std::string const &k) const;
};
Input parse(QStringList const &args);
std::vector<std::string> command_paths();
J defaults();
J schema();
std::string builtin_profiles();
struct Config {
  J value, warnings = J::array();
  explicit Config(std::string const &override_path);
};
fs::path config_path();
J profile_command(Input const &, Config const &);
J config_set_command(Input const &);
void validate_profile(std::vector<std::string> const &args, bool build = false);
std::string engine_path();
fs::path runtime_mount_root();
struct Run {
  int code;
  QByteArray out, err;
  J payload;
};
Run engine(std::string const &, std::vector<std::string> const &,
           std::optional<fs::path> output = {});
Run execute(std::string const &, std::vector<std::string> const &,
            std::optional<fs::path> output = {},
            std::optional<fs::path> cwd = {});
void require_success(Run const &);
J inspect(fs::path const &);
J listing(J const &tree);
J read_command(Input const &, Config const &);
J build_command(Input const &, Config const &);
J checksum_command(Input const &, Config const &);
J recommend(Input const &);
J diagnostics(Input const &, Config const &);
J mount_command(Input const &);
std::string hash(std::string const &);
std::string resolved_path(fs::path const &);
fs::path output_path(fs::path const &);
bool path_exists(fs::path const &);
void publish(fs::path const &staged, fs::path const &dest, bool overwrite);
void atomic_write(fs::path const &, std::string const &);
struct Temp {
  fs::path path;
  explicit Temp(fs::path const &directory);
  ~Temp();
  Temp(Temp const &) = delete;
};
struct Lock {
  int fd = -1;
  explicit Lock(fs::path const &path);
  ~Lock();
  Lock(Lock const &) = delete;
};
J envelope(std::string const &, J const &image, J const &details,
           J const &warnings = J::array(), Error const *error = nullptr);
} // namespace ctl
