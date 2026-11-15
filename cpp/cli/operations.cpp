// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include <QCryptographicHash>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/fs.h>
#include <set>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace ctl {
namespace {

constexpr size_t kUsageMaxDepth = 2;
constexpr size_t kUsageMaxChildren = 18;

struct UsageNode {
  uint64_t bytes = 0;
  uint64_t files = 0;
  uint64_t directories = 0;
  J json;
};

UsageNode summarize_usage(J const &node, std::string const &parent,
                          size_t depth) {
  auto name = node.value("name", "");
  bool directory = node.value("type", "") == "directory";
  auto path = parent.empty() ? "/" : parent + (parent == "/" ? "" : "/") + name;
  UsageNode out;
  out.files = directory ? 0 : 1;
  out.directories = directory && depth > 0 ? 1 : 0;
  if (!directory && node.contains("size") && node["size"].is_number_unsigned())
    out.bytes = node["size"].get<uint64_t>();

  std::vector<UsageNode> children;
  if (directory && node.contains("inodes") && node["inodes"].is_array()) {
    children.reserve(node["inodes"].size());
    for (auto const &child : node["inodes"]) {
      auto summary = summarize_usage(child, path, depth + 1);
      out.bytes += summary.bytes;
      out.files += summary.files;
      out.directories += summary.directories;
      children.push_back(std::move(summary));
    }
  }

  out.json = {{"name", name},
              {"path", path},
              {"type", directory ? "directory" : "file"},
              {"size", out.bytes},
              {"files", out.files},
              {"directories", out.directories}};
  if (directory && depth < kUsageMaxDepth) {
    std::sort(children.begin(), children.end(), [](auto const &a, auto const &b) {
      if (a.bytes != b.bytes)
        return a.bytes > b.bytes;
      return a.json.value("name", "") < b.json.value("name", "");
    });
    J visible = J::array();
    size_t keep = children.size();
    if (keep > kUsageMaxChildren)
      keep = kUsageMaxChildren - 1;
    for (size_t i = 0; i < keep; ++i)
      visible.push_back(children[i].json);
    if (keep < children.size()) {
      UsageNode other;
      for (size_t i = keep; i < children.size(); ++i) {
        other.bytes += children[i].bytes;
        other.files += children[i].files;
        other.directories += children[i].directories;
      }
      visible.push_back({{"name", "Other"},
                         {"path", nullptr},
                         {"type", "group"},
                         {"size", other.bytes},
                         {"files", other.files},
                         {"directories", other.directories},
                         {"grouped_entries", children.size() - keep}});
    }
    out.json["children"] = std::move(visible);
  }
  return out;
}

J usage_summary(J const &tree) {
  if (!tree.contains("root"))
    throw Error(9, "image metadata has no root");
  auto root = summarize_usage(tree["root"], "", 0);
  return {{"schema", "dwarfsctl.usage.v1"},
          {"total_size", root.bytes},
          {"files", root.files},
          {"directories", root.directories},
          {"max_depth", kUsageMaxDepth},
          {"max_children", kUsageMaxChildren},
          {"root", std::move(root.json)}};
}

} // namespace

J inspect(fs::path const &image) {
  if (!fs::is_regular_file(image))
    throw Error(4, "image does not exist or is not a regular file: " +
                       image.string());
  auto r = engine("inspect", {image.string()});
  try {
    require_success(r);
  } catch (Error const &e) {
    throw Error(e.code, image.string() + ": " + e.what(), e.details);
  }
  return J::parse(r.out.toStdString());
}
J listing(J const &tree) {
  if (!tree.contains("root"))
    throw Error(9, "image metadata has no root");
  J out = J::array();
  std::function<void(J const &, std::string const &)> walk =
      [&](J const &node, std::string const &parent) {
        auto name = node.value("name", "");
        bool dir = node.value("type", "") == "directory";
        auto path =
            parent.empty() ? "/" : parent + (parent == "/" ? "" : "/") + name;
        out.push_back(
            {{"name", name},
             {"path", path},
             {"is_dir", dir},
             {"size", dir ? J(nullptr) : node.value("size", J(nullptr))},
             {"mode", node.value("mode", J(nullptr))}});
        if (dir && node.contains("inodes"))
          for (auto const &child : node["inodes"])
            walk(child, path);
      };
  walk(tree["root"], "");
  std::sort(out.begin(), out.end(),
            [](auto const &a, auto const &b) { return a["path"] < b["path"]; });
  return out;
}
J read_command(Input const &in, Config const &) {
  fs::path image = in.positions[0];
  if (!fs::is_regular_file(image))
    throw Error(4, "image is not a regular file: " + image.string());
  if (in.command == "cat") {
    auto inner = in.positions[1];
    fs::path dest = output_path(in.positions[2]);
    if (path_exists(dest) && !in.has("force"))
      throw Error(4, "destination exists; pass --force to overwrite");
    if (inner.empty() || inner == "/")
      throw Error(4, "expected a file path inside the image");
    for (auto const &p : fs::path(inner))
      if (p == "." || p == "..")
        throw Error(4, "dot path components are not allowed");
    Temp tmp(dest.parent_path());
    auto r = engine("cat", {image.string(), inner}, tmp.path);
    require_success(r);
    auto bytes = fs::file_size(tmp.path);
    publish(tmp.path, dest, in.has("force"));
    auto first = inner.find_first_not_of('/');
    return {{"inner_path", inner},
            {"dest", in.positions[2]},
            {"bytes", bytes},
            {"pattern_used", inner.substr(first)},
            {"tool", {{"tool", r.payload}}}};
  }
  if (in.command == "extract") {
    fs::path dest = output_path(in.positions[1]);
    // Extraction can write many paths. Never merge into an existing directory:
    // a same-named entry in the image could otherwise replace a user's file.
    // Build the complete tree beside the requested path and publish it with a
    // no-replace rename only after the engine succeeds.
    if (path_exists(dest))
      throw Error(4, "extraction destination already exists: " + dest.string());
    auto staging = std::make_unique<QTemporaryDir>(
        qs((dest.parent_path() / ".dwarfsctl-extract-XXXXXX").string()));
    if (!staging->isValid())
      throw Error(9, "cannot create extraction staging directory beside " +
                         dest.string());
    fs::path extract_dest = staging->path().toStdString();
    auto args = in.many("arg");
    Run r;
    if (args.empty())
      r = engine("extract", {image.string(), extract_dest.string()});
    else {
      args.insert(args.begin(),
                  {"-i", resolved_path(image), "-o", extract_dest.string()});
      r = engine("dwarfsextract", args);
    }
    require_success(r);
    if (::syscall(SYS_renameat2, AT_FDCWD, extract_dest.c_str(), AT_FDCWD,
                  dest.c_str(), RENAME_NOREPLACE) != 0)
      throw Error(errno == EEXIST ? 4 : 9,
                  "cannot publish extraction directory " + dest.string() +
                      ": " + std::strerror(errno));
    return {{"dest", in.positions[1]}, {"tool", r.payload}};
  }
  if (in.command == "fsck") {
    std::vector<std::string> args{"--json", "--detail=7", "-i", image.string()};
    auto extra = in.many("arg");
    args.insert(args.end(), extra.begin(), extra.end());
    auto r = engine("dwarfsck", args);
    require_success(r);
    if (!r.out.isEmpty())
      r.payload["json"] = J::parse(r.out.toStdString());
    return {{"tool", r.payload}};
  }
  auto tree = inspect(image);
  if (in.command == "usage")
    return usage_summary(tree);
  auto entries = listing(tree);
  if (in.command == "peek") {
    uint64_t files = 0, dirs = 0, bytes = 0;
    J sample = J::array();
    size_t count = 0;
    for (auto const &e : entries) {
      if (e["is_dir"] == true) {
        if (e["path"] != "/")
          ++dirs;
      } else {
        ++files;
        if (e["size"].is_number_unsigned())
          bytes += e["size"].get<uint64_t>();
      }
      if (e["path"] != "/") {
        ++count;
        if (sample.size() < 200) {
          J v = e;
          v.erase("mode");
          if (v["size"].is_null())
            v.erase("size");
          sample.push_back(v);
        }
      }
    }
    tree.erase("root");
    return {{"format", tree},
            {"counts",
             {{"files", files}, {"directories", dirs}, {"total_size", bytes}}},
            {"sample", sample},
            {"sample_limit", 200},
            {"sample_truncated", count > 200}};
  }
  if (!in.has("path"))
    return {{"entries", entries}};
  auto path = in.get("path");
  while (path.size() > 1 && path.back() == '/')
    path.pop_back();
  auto prefix = path == "/" ? "/" : path + '/';
  J selected = J::array();
  bool exists = false;
  for (auto const &e : entries) {
    auto p = arg(e["path"]);
    if (p == path)
      exists = true;
    if (in.has("entry-only")) {
      if (p == path)
        selected.push_back(e);
    } else if (in.has("children-only")) {
      if (p.starts_with(prefix) && p != path &&
          p.substr(prefix.size()).find('/') == std::string::npos)
        selected.push_back(e);
    } else if (p == path || p.starts_with(prefix))
      selected.push_back(e);
  }
  if (!exists && !in.has("children-only"))
    throw Error(4, "no such path in image: " + path);
  return {{"entries", selected}};
}
J build_command(Input const &in, Config const &cfg) {
  cancellation_point();
  fs::path source = in.positions[0];
  if (!fs::is_directory(source))
    throw Error(4, "source must be an existing directory");
  fs::path dest = output_path(in.get("output"));
  Lock lock(runtime_mount_root() / "locks" / (hash(dest.string()) + ".lock"));
  bool overwrite =
      in.has("force") || in.has("overwrite-output") || in.has("yes");
  if (path_exists(dest) && !overwrite)
    throw Error(2, "output exists; pass --force or --overwrite-output");
  auto args = cfg.value["mkdwarfs"]["args"].get<std::vector<std::string>>();
  auto extra = in.many("mkdwarfs-arg");
  if (in.has("compression"))
    extra.push_back("--compression=" + in.get("compression"));
  validate_profile(args, true);
  validate_profile(extra, true);
  args.insert(args.end(), extra.begin(), extra.end());
  std::vector<std::string> actual{"-i", resolved_path(source), "-o",
                                  dest.string()};
  actual.insert(actual.end(), args.begin(), args.end());
  if (overwrite)
    actual.push_back("--force");
  J argv = {engine_path(), "mkdwarfs"};
  for (auto const &a : actual)
    argv.push_back(a);
  J details = {
      {"tool",
       {{"name", "mkdwarfs"}, {"found", true}, {"path", engine_path()}}},
      {"source_dir", source.string()},
      {"output_image", dest.string()},
      {"argv", argv},
      {"working_dir", nullptr},
      {"stdout", ""},
      {"stderr", ""},
      {"status_code", 0},
      {"output_verified", true}};
  if (in.has("dry-run"))
    return details;
  // Build beside the destination and publish only a verified, readable image.
  // A failed writer cannot destroy an earlier image, even with --force.
  const auto started = std::chrono::steady_clock::now();
  std::cerr << J({{"event", "build_progress"}, {"phase", "preparing"}}).dump()
            << std::endl;
  Temp temp(dest.parent_path());
  actual[3] = temp.path.string();
  std::erase(actual, "--force");
  std::erase(actual, "-f");
  actual.push_back("--force");
  auto r = engine("mkdwarfs", actual);
  details["argv"] = r.payload["argv"];
  details["stdout"] = r.payload.value("stdout", "");
  details["stderr"] = r.payload.value("stderr", "");
  details["status_code"] = r.code;
  if (r.code || !fs::is_regular_file(temp.path) ||
      fs::file_size(temp.path) == 0) {
    details["output_verified"] = false;
    throw Error(9, "image build failed", details);
  }
  std::cerr << J({{"event", "build_progress"}, {"phase", "verifying"}}).dump()
            << std::endl;
  cancellation_point();
  const auto metadata = inspect(temp.path);
  uint64_t files = 0, original = 0;
  std::function<void(J const &)> count = [&](J const &entry) {
    if (entry.value("type", "") == "file") {
      ++files;
      original += entry.value("size", uint64_t(0));
    }
    if (entry.contains("inodes"))
      for (auto const &child : entry["inodes"])
        count(child);
  };
  count(metadata.at("root"));
  details["summary"] = {
      {"files", files},
      {"original_bytes", original},
      {"image_bytes", fs::file_size(temp.path)},
      {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - started)
                         .count()}};
  publish(temp.path, dest, overwrite);
  return details;
}
J checksum_command(Input const &in, Config const &) {
  auto image = in.positions[0], algo = in.get("algo");
  if (in.command == "checksum export") {
    std::vector<std::string> args{"-i", image, "--checksum=" + algo};
    std::optional<Temp> tmp;
    std::optional<fs::path> output;
    if (in.has("output")) {
      fs::path dest = output_path(in.get("output"));
      std::error_code ec;
      if (dest == fs::path(resolved_path(image)) ||
          fs::equivalent(dest, image, ec))
        throw Error(2, "checksum output must not replace the image");
      tmp.emplace(dest.parent_path());
      output = tmp->path;
    }
    auto r = engine("dwarfsck", args, output);
    require_success(r);
    J result = {{"tool", r.payload}};
    if (tmp) {
      publish(tmp->path, output_path(in.get("output")), true);
      result["output"] = in.get("output");
    }
    return result;
  }
  static const std::map<std::string, std::string> verifiers{
      {"md5", "md5sum"},       {"sha1", "sha1sum"},     {"sha224", "sha224sum"},
      {"sha256", "sha256sum"}, {"sha384", "sha384sum"}, {"sha512", "sha512sum"},
      {"blake2b", "b2sum"}};
  if (!verifiers.contains(algo))
    throw Error(2, "unsupported checksum verification algorithm: " + algo);
  auto program = str(QStandardPaths::findExecutable(qs(verifiers.at(algo))));
  if (program.empty())
    throw Error(3, "checksum verifier unavailable: " + verifiers.at(algo));
  fs::path input = resolved_path(in.get("input"));
  if (!fs::is_regular_file(input))
    throw Error(4, "checksum input does not exist");
  auto cwd = in.has("dir") ? fs::path(resolved_path(in.get("dir")))
                           : input.parent_path();
  auto r = execute(program, {"--check", input.string()}, {}, cwd);
  r.payload["tool"] = verifiers.at(algo);
  J details = {{"tool", r.payload}, {"verified", r.code == 0}};
  if (r.code)
    throw Error(9, "checksum verification failed", details);
  return details;
}
} // namespace ctl
