// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include <set>
namespace ctl {
struct CommandSpec {
  int min, max;
  std::set<std::string> values, flags, repeats;
};
static const std::map<std::string, CommandSpec> commands{
    {"build",
     {1,
      1,
      {"output", "compression", "mkdwarfs-arg"},
      {"force", "overwrite-output", "yes", "dry-run"},
      {"mkdwarfs-arg"}}},
    {"ls", {1, 1, {"path"}, {"children-only", "entry-only"}, {}}},
    {"cat", {3, 3, {}, {"force"}, {}}},
    {"peek", {1, 1, {}, {}, {}}},
    {"usage", {1, 1, {}, {}, {}}},
    {"extract", {2, 2, {"arg"}, {}, {"arg"}}},
    {"fsck", {1, 1, {"arg"}, {}, {"arg"}}},
    {"doctor", {0, 0, {}, {}, {}}},
    {"status", {0, 1, {}, {}, {}}},
    {"mount", {1, 1, {}, {}, {}}},
    {"mount list", {0, 0, {}, {}, {}}},
    {"mount status", {1, 1, {}, {}, {}}},
    {"mount registry-path", {0, 0, {}, {}, {}}},
    {"unmount", {0, 1, {"mountpoint", "image"}, {}, {}}},
    {"unmount all", {0, 0, {}, {}, {}}},
    {"unmount last", {0, 0, {}, {}, {}}},
    {"checksum export", {1, 1, {"algo", "output"}, {}, {}}},
    {"checksum verify", {1, 1, {"algo", "input", "dir"}, {}, {}}},
    {"config show", {0, 0, {"section"}, {}, {}}},
    {"config set", {2, 14, {}, {}, {}}},
    {"config mkdwarfs-profiles list", {0, 0, {}, {"all"}, {}}},
    {"config mkdwarfs-profiles get", {1, 1, {}, {}, {}}},
    {"config mkdwarfs-profiles set", {1, 1, {"arg"}, {}, {"arg"}}},
    {"config mkdwarfs-profiles delete", {1, 1, {}, {}, {}}},
    {"config mkdwarfs-profiles validate", {0, 0, {"arg"}, {}, {"arg"}}},
    {"mkdwarfs schema", {0, 0, {}, {"include-long-help"}, {}}},
    {"mkdwarfs recommend", {1, 1, {"goal", "cache"}, {}, {}}},
    {"tools schema", {0, 0, {}, {}, {}}}};
std::vector<std::string> command_paths() {
  std::vector<std::string> out;
  for (auto const &[c, s] : commands)
    out.push_back(c);
  return out;
}
Input parse(QStringList const &raw) {
  Input in;
  std::vector<std::string> tokens;
  // Consume global options while respecting pass-through values beginning '-'.
  const std::set<std::string> valued{
      "--arg",  "--mkdwarfs-arg", "--output", "--compression", "--compress",
      "--path", "--mountpoint",   "--image",  "--algo",        "--input",
      "--dir",  "--section",      "--goal",   "--cache"};
  bool literal = false;
  for (int i = 0; i < raw.size(); ++i) {
    auto t = str(raw[i]);
    if (t == "--")
      literal = true;
    if (literal) {
      tokens.push_back(t);
      continue;
    }
    auto eq = t.find('=');
    auto k = t.substr(0, eq);
    if (k == "--config") {
      auto v = eq != std::string::npos
                   ? t.substr(eq + 1)
                   : (i + 1 < raw.size()
                          ? str(raw[++i])
                          : throw Error(2, "--config requires a value"));
      in.options["config"] = {v};
    } else if (t == "--verbose")
      in.options["verbose"] = {"true"};
    else {
      tokens.push_back(t);
      if (valued.contains(k) && eq == std::string::npos && i + 1 < raw.size())
        tokens.push_back(str(raw[++i]));
    }
  }
  if (tokens.empty())
    throw Error(2, "expected a command; use --help");
  size_t count = 0;
  for (auto const &[name, spec] : commands) {
    std::string joined;
    for (size_t n = 0; n < std::min<size_t>(3, tokens.size()); ++n) {
      if (n)
        joined += ' ';
      joined += tokens[n];
      if (joined == name && n + 1 > count) {
        in.command = name;
        count = n + 1;
      }
    }
  }
  if (!count)
    throw Error(2, "unknown or incomplete command: " + tokens[0]);
  auto const &spec = commands.at(in.command);
  bool positional = false;
  for (size_t i = count; i < tokens.size(); ++i) {
    auto t = tokens[i];
    if (!positional && t == "--") {
      positional = true;
      continue;
    }
    if (!positional && t.starts_with('-')) {
      auto eq = t.find('=');
      auto key = t.substr(0, eq);
      if (!key.starts_with("--"))
        throw Error(2, "unknown option: " + key);
      key.erase(0, 2);
      if (key == "compress")
        key = "compression";
      if (!spec.values.contains(key) && !spec.flags.contains(key))
        throw Error(2, "unknown option --" + key);
      if (in.has(key) && !spec.repeats.contains(key))
        throw Error(2, "duplicate option --" + key);
      if (spec.flags.contains(key)) {
        if (eq != std::string::npos)
          throw Error(2, "flag takes no value: " + key);
        in.options[key] = {"true"};
      } else {
        auto value = eq != std::string::npos
                         ? t.substr(eq + 1)
                         : (i + 1 < tokens.size()
                                ? tokens[++i]
                                : throw Error(2, "missing value for --" + key));
        in.options[key].push_back(value);
      }
    } else
      in.positions.push_back(t);
  }
  if (in.positions.size() < static_cast<size_t>(spec.min) ||
      in.positions.size() > static_cast<size_t>(spec.max))
    throw Error(2, "wrong number of arguments for " + in.command);
  if (in.command == "config set" && in.positions.size() % 2 != 0)
    throw Error(2, "config set requires KEY VALUE pairs");
  if (in.command == "build" && !in.has("output"))
    throw Error(2, "--output is required");
  if (in.command.starts_with("checksum") && !in.has("algo"))
    throw Error(2, "--algo is required");
  if (in.command == "checksum verify" && !in.has("input"))
    throw Error(2, "--input is required");
  if (in.has("entry-only") && (!in.has("path") || in.has("children-only")))
    throw Error(
        2, "--entry-only requires --path and conflicts with --children-only");
  if (in.command == "unmount" &&
      in.positions.size() + in.has("image") + in.has("mountpoint") != 1)
    throw Error(2, "specify one unmount target");
  return in;
}
} // namespace ctl
