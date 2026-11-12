// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <toml++/toml.hpp>
namespace ctl {
static toml::table read_table(fs::path const &p);
J defaults() {
  return {{"tools",
           {{"mkdwarfs", "mkdwarfs"},
            {"dwarfs", "dwarfs"},
            {"dwarfsck", "dwarfsck"},
            {"dwarfsextract", "dwarfsextract"}}},
          {"mkdwarfs", {{"args", {"--compression=zstd:level=9"}}}},
          {"creation",
           {{"default_mode", "recommend"},
            {"default_profile", "default"},
            {"recommendation_goal", "balanced"}}},
          {"ui",
           {{"open_after_mount", true},
            {"command_timeout_ms", 300000},
            {"build_timeout_ms", 0}}}};
}

J config_set_command(Input const &in) {
  static const std::map<std::string, std::string> types{
      {"ui.open_after_mount", "bool"},
      {"ui.command_timeout_ms", "uint"},
      {"ui.build_timeout_ms", "uint"},
      {"creation.default_mode", "mode"},
      {"creation.default_profile", "string"},
      {"creation.recommendation_goal", "goal"},
  };
  auto path = config_path();
  Lock lock(path.string() + ".lock");
  auto table = read_table(path);
  // Versions through 0.12.19 wrote this internal preference whenever the
  // Settings dialog was saved. Image sidecars are retired, so remove the stale
  // key when another supported preference is next saved.
  if (auto places = table["places"].as_table()) {
    places->erase("write_sidecar");
    if (places->empty())
      table.erase("places");
  }
  J written = J::object();
  for (size_t i = 0; i < in.positions.size(); i += 2) {
    auto const &key = in.positions[i];
    auto const &text = in.positions[i + 1];
    auto type = types.find(key);
    if (type == types.end())
      throw Error(2, "unsupported preference: " + key);

    J parsed;
    if (type->second == "bool") {
      if (text == "true")
        parsed = true;
      else if (text == "false")
        parsed = false;
      else
        throw Error(2, "boolean preference requires true or false: " + key);
    } else if (type->second == "uint") {
      if (text.empty() ||
          !std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return std::isdigit(c);
          }))
        throw Error(2, "integer preference requires milliseconds: " + key);
      try {
        auto value = std::stoll(text);
        if (value < 0 || value > UINT32_MAX)
          throw Error(2, "config value out of range: " + key);
        parsed = value;
      } catch (std::invalid_argument const &) {
        throw Error(2, "integer preference requires milliseconds: " + key);
      } catch (std::out_of_range const &) {
        throw Error(2, "config value out of range: " + key);
      }
    } else {
      if (text.empty())
        throw Error(2, "preference cannot be empty: " + key);
      if (type->second == "mode" && text != "recommend" && text != "profile")
        throw Error(2, "creation.default_mode must be recommend or profile");
      if (type->second == "goal") {
        static const std::set<std::string> goals{
            "balanced", "smallest", "fastest-browse", "fast-build"};
        if (!goals.contains(text))
          throw Error(2, "unsupported recommendation goal: " + text);
      }
      parsed = text;
    }

    auto dot = key.find('.');
    auto section_name = key.substr(0, dot);
    auto item_name = key.substr(dot + 1);
    auto section = table[section_name].as_table();
    if (!section) {
      table.insert_or_assign(section_name, toml::table{});
      section = table[section_name].as_table();
    }
    if (parsed.is_boolean())
      section->insert_or_assign(item_name, parsed.get<bool>());
    else if (parsed.is_number_integer())
      section->insert_or_assign(item_name, parsed.get<int64_t>());
    else
      section->insert_or_assign(item_name, parsed.get<std::string>());
    written[key] = parsed;
  }
  std::ostringstream out;
  out << table;
  atomic_write(path, out.str());
  return {{"preferences", written}, {"path", path.string()}};
}
fs::path config_path() {
  if (auto v = std::getenv("XDG_CONFIG_HOME"))
    if (fs::path(v).is_absolute())
      return fs::path(v) / "dwarfs-dolphin/config.toml";
  if (auto v = std::getenv("HOME"))
    return fs::path(v) / ".config/dwarfs-dolphin/config.toml";
  throw Error(2, "HOME or absolute XDG_CONFIG_HOME is required");
}
static toml::table read_table(fs::path const &p) {
  if (!fs::exists(p))
    return {};
  try {
    return toml::parse_file(p.string());
  } catch (toml::parse_error const &e) {
    throw Error(2, "invalid config " + p.string() + ": " +
                       std::string(e.description()));
  }
}
static J to_json(toml::table const &t) {
  std::ostringstream out;
  out << toml::json_formatter(t);
  return J::parse(out.str());
}
static void merge(J &base, J const &layer) {
  if (base.is_object() && layer.is_object())
    for (auto const &[k, v] : layer.items())
      merge(base[k], v);
  else
    base = layer;
}
static void warn_unknown(J const &layer, J const &expected, J &warnings,
                         std::string const &source,
                         std::string const &prefix = "") {
  if (!layer.is_object())
    return;
  for (auto const &[k, v] : layer.items()) {
    auto key = prefix.empty() ? k : prefix + '.' + k;
    if (key == "mkdwarfs_profiles")
      continue;
    if (!expected.contains(k))
      warnings.push_back(
          {{"code", "config_unknown_key"},
           {"message", "unrecognised config key '" + key + "' in " + source +
                           "; it has no effect"}});
    else if (expected[k].is_object())
      warn_unknown(v, expected[k], warnings, source, key);
  }
}
static void validate_types(J &merged, J const &expected,
                           std::string const &path = "") {
  if (!merged.is_object())
    throw Error(2, "config table required: " + path);
  for (auto const &[k, v] : expected.items()) {
    auto key = path.empty() ? k : path + '.' + k;
    if (v.is_object())
      validate_types(merged[k], v, key);
    else {
      bool valid = v.is_number() ? merged[k].is_number_integer()
                                 : merged[k].type() == v.type();
      if (!valid)
        throw Error(2, "wrong config type for " + key);
      if (v.is_array())
        for (auto const &item : merged[k])
          if (!item.is_string())
            throw Error(2, "string array required: " + key);
      if (v.is_number() && (merged[k].get<int64_t>() < 0 ||
                            merged[k].get<uint64_t>() > UINT32_MAX))
        throw Error(2, "config value out of range: " + key);
    }
  }
  for (auto it = merged.begin(); it != merged.end();)
    if (!expected.contains(it.key()))
      it = merged.erase(it);
    else
      ++it;
}
Config::Config(std::string const &override_path) : value(defaults()) {
  std::vector<fs::path> layers{"/etc/dwarfs-dolphin/config.toml",
                               config_path()};
  if (!override_path.empty()) {
    if (!fs::exists(override_path))
      throw Error(2, "config override does not exist: " + override_path);
    layers.emplace_back(override_path);
  }
  for (auto const &p : layers) {
    auto layer = to_json(read_table(p));
    warn_unknown(layer, defaults(), warnings, p.string());
    merge(value, layer);
  }
  validate_types(value, defaults());
  auto const mode = value["creation"]["default_mode"].get<std::string>();
  if (mode != "recommend" && mode != "profile")
    throw Error(2, "creation.default_mode must be recommend or profile");
  static const std::set<std::string> goals{
      "balanced", "smallest", "fastest-browse", "fast-build"};
  auto const goal = value["creation"]["recommendation_goal"].get<std::string>();
  if (!goals.contains(goal))
    throw Error(2, "unsupported recommendation goal: " + goal);
  // Kept for config compatibility; embedded engine is always authoritative.
  for (auto const &[k, v] : value["tools"].items())
    if (v != k)
      warnings.push_back({{"code", "config_external_tool_ignored"},
                          {"message", "tools." + k +
                                          " is retained for compatibility; the "
                                          "pinned embedded engine is used"}});
}
static J profile_map(J const &doc) {
  J out = J::object();
  if (!doc.contains("mkdwarfs_profiles"))
    return out;
  auto const &profiles = doc["mkdwarfs_profiles"];
  if (!profiles.is_object())
    throw Error(2, "mkdwarfs_profiles must be a table");
  for (auto const &[name, v] : profiles.items()) {
    J args = v.is_array() ? v : v.value("args", J::array());
    if (!args.is_array())
      throw Error(2, "profile args must be a string array");
    for (auto const &a : args)
      if (!a.is_string())
        throw Error(2, "profile arguments must be strings");
    out[name] = {
        {"args", args},
        {"read_only", v.is_object() ? v.value("read_only", false) : false}};
  }
  return out;
}
J profile_command(Input const &in, Config const &) {
  if (in.command.ends_with("validate")) {
    validate_profile(in.many("arg"));
    return {{"valid", true}};
  }
  auto path = config_path();
  bool mutation = in.command.ends_with("set") || in.command.ends_with("delete");
  std::unique_ptr<Lock> lock;
  if (mutation)
    lock = std::make_unique<Lock>(path.string() + ".lock");
  auto table = read_table(path);
  auto user = profile_map(to_json(table));
  auto system = fs::exists("/etc/dwarfs-dolphin/config.toml")
                    ? read_table("/etc/dwarfs-dolphin/config.toml")
                    : toml::parse(builtin_profiles());
  auto merged = profile_map(to_json(system));
  for (auto &[name, v] : merged.items())
    v["origin"] = "system";
  for (auto [name, v] : user.items()) {
    v["origin"] = "user";
    merged[name] = v;
  }
  if (in.command.ends_with("list")) {
    J names = J::array(), meta = J::array();
    for (auto const &[name, v] : merged.items())
      if (in.has("all") || v["origin"] == "user") {
        names.push_back(name);
        meta.push_back({{"name", name},
                        {"origin", v["origin"]},
                        {"read_only", v["read_only"]}});
      }
    return {{"profiles", names}, {"profiles_meta", meta}};
  }
  auto name = in.positions.at(0);
  if (in.command.ends_with("get")) {
    if (name == "default" || name == "Default")
      return {{"profile", {{"name", name}, {"args", J::array()}}}};
    if (!merged.contains(name))
      throw Error(4, "unknown profile: " + name);
    return {{"profile", {{"name", name}, {"args", merged[name]["args"]}}}};
  }
  if (qs(name).trimmed().isEmpty() || name.starts_with('@') ||
      name == "default" || name == "Default")
    throw Error(2, "reserved or empty profile name");
  if (merged.contains(name) &&
      (merged[name]["origin"] == "system" || merged[name]["read_only"] == true))
    throw Error(2, "cannot modify system or read-only profile");
  auto profiles = table["mkdwarfs_profiles"].as_table();
  if (!profiles) {
    table.insert_or_assign("mkdwarfs_profiles", toml::table{});
    profiles = table["mkdwarfs_profiles"].as_table();
  }
  J result;
  if (in.command.ends_with("delete")) {
    if (!user.contains(name))
      throw Error(4, "unknown profile: " + name);
    profiles->erase(name);
    result = {{"deleted", name}};
  } else {
    auto args = in.many("arg");
    validate_profile(args);
    toml::array a;
    for (auto const &v : args)
      a.push_back(v);
    profiles->insert_or_assign(name, toml::table{{"args", std::move(a)}});
    result = {{"profile", {{"name", name}, {"args", args}}}};
  }
  std::ostringstream out;
  out << table;
  atomic_write(path, out.str());
  return result;
}
} // namespace ctl
