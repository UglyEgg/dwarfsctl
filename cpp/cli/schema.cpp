// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include <cmath>
#include <set>
namespace ctl {
void validate_profile(std::vector<std::string> const &args, bool build) {
  std::map<std::string, J> specs;
  auto document = schema();
  for (auto const &c : document["categories"])
    for (auto const &o : c["options"])
      specs[arg(o["opt"])] = o;
  const std::map<std::string, std::string> aliases{
      {"-l", "--compress-level"}, {"-S", "--block-size-bits"},
      {"-N", "--num-workers"},    {"-L", "--memory-limit"},
      {"-F", "--filter"},         {"-B", "--max-lookback-blocks"},
      {"-W", "--window-size"},    {"-w", "--window-step"},
      {"-C", "--compression"},    {"-P", "--pack-metadata"}};
  auto canonical = [&](std::string s) {
    return aliases.contains(s) ? aliases.at(s) : s;
  };
  const std::set<std::string> forbidden{
      "-i", "--input", "-o", "--output",    "--man",
      "-h", "--help",  "-H", "--long-help", "--version"};
  auto option = [&](std::string const &s) {
    auto k = canonical(s.substr(0, s.find('=')));
    return specs.contains(k) || forbidden.contains(k) || k == "--force" ||
           k == "-f";
  };
  for (size_t i = 0; i < args.size(); ++i) {
    auto token = args[i];
    auto eq = token.find('=');
    auto key = canonical(token.substr(0, eq));
    if (forbidden.contains(key))
      throw Error(2, "option is controlled by dwarfsctl: " + key);
    if (key == "--force" || key == "-f") {
      if (!build)
        throw Error(2, "force is build-level, not a profile option");
      continue;
    }
    if (!specs.contains(key))
      throw Error(2, "unknown mkdwarfs option: " + key);
    auto spec = specs.at(key);
    auto kind = spec.value("kind", "");
    bool optional =
        spec.value("optional_value", false) || kind.starts_with("Optional");
    if (kind == "Flag") {
      if (eq != std::string::npos)
        throw Error(2, "flag takes no value: " + key);
      continue;
    }
    std::string value;
    if (eq != std::string::npos)
      value = token.substr(eq + 1);
    else if (i + 1 < args.size() && !option(args[i + 1]))
      value = args[++i];
    else if (optional)
      continue;
    else
      throw Error(2, "missing value for " + key);
    if (value.empty())
      throw Error(2, "empty value for " + key);
    if (kind == "Int" || kind == "Double") {
      double n = 0;
      size_t consumed = 0;
      try {
        n = std::stod(value, &consumed);
      } catch (std::exception const &) {
        throw Error(2, "invalid number for " + key);
      }
      if (consumed != value.size() || !std::isfinite(n) ||
          (kind == "Int" && std::floor(n) != n))
        throw Error(2, "invalid number for " + key);
      auto low =
               spec.value(kind == "Int" ? "min_int" : "min_double", J(nullptr)),
           high =
               spec.value(kind == "Int" ? "max_int" : "max_double", J(nullptr));
      if ((low.is_number() && n < low.get<double>()) ||
          (high.is_number() && n > high.get<double>()))
        throw Error(2, "value out of range for " + key);
    }
    if (kind == "Enum" || kind == "OptionalEnum") {
      auto values = spec["enum_values"];
      if (std::find(values.begin(), values.end(), J(value)) == values.end())
        throw Error(2, "invalid value for " + key);
    }
    if (kind == "CompressionSpec" || key == "--compression" ||
        key == "--history-compression") {
      auto split = value.find("::");
      auto compression =
          split == std::string::npos ? value : value.substr(split + 2);
      // Some compressor constructors defer range checks until actual writes.
      // Preserve eager profile validation without creating an image.
      static const std::map<std::string, std::pair<int64_t, int64_t>> limits{
          {"zstd:level", {-131072, 22}}, {"lz4hc:level", {0, 12}},
          {"brotli:quality", {0, 11}},   {"brotli:lgwin", {10, 30}},
          {"flac:level", {0, 8}},        {"ricepp:block_size", {16, 512}},
          {"lzma:level", {0, 9}},        {"lzma:dict_size", {12, 30}},
          {"lzma:nice", {0, 273}},       {"lzma:depth", {0, 4294967295LL}}};
      auto colon = compression.find(':');
      auto algorithm = compression.substr(0, colon);
      while (colon != std::string::npos) {
        auto next = compression.find(':', colon + 1);
        auto part = compression.substr(
            colon + 1, next == std::string::npos ? next : next - colon - 1);
        auto equal = part.find('=');
        auto parameter = algorithm + ":" + part.substr(0, equal);
        if (equal != std::string::npos && limits.contains(parameter)) {
          int64_t number;
          size_t count = 0;
          auto text = part.substr(equal + 1);
          try {
            number = std::stoll(text, &count);
          } catch (std::exception const &) {
            throw Error(2, "invalid compressor number: " + part);
          }
          auto [low, high] = limits.at(parameter);
          if (count != text.size() || number < low || number > high)
            throw Error(2, "compressor parameter out of range: " + part);
        }
        colon = next;
      }
      auto result = engine("validate-compression", {compression});
      if (result.code)
        throw Error(2, "invalid compression specification: " + value + ": " +
                           str(QString::fromUtf8(result.err.left(1024))));
    }
    if (key == "--order") {
      auto split = value.find("::");
      auto order = split == std::string::npos ? value : value.substr(split + 2);
      const std::set<std::string> orders{"none",       "path",     "revpath",
                                         "similarity", "nilsimsa", "explicit"};
      if (!orders.contains(order))
        throw Error(2, "invalid file order: " + value);
    }
  }
}
} // namespace ctl
