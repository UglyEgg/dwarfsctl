// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include <QCryptographicHash>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <charconv>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/fs.h>
#include <map>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace ctl {
namespace {

fs::path checked_preview_stage(fs::path const &raw, fs::path const &dest)
{
  const auto stage = fs::absolute(raw).lexically_normal();
  struct stat info {};
  if (!stage.filename().string().starts_with(".dwarfsctl-preview-") ||
      ::lstat(stage.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) ||
      info.st_uid != ::geteuid() || (info.st_mode & 077) != 0)
    throw Error(4, "preview staging directory must be private and owned by the caller");
  std::error_code ec;
  const auto canonical = fs::canonical(stage, ec);
  if (ec || canonical.parent_path() != dest.parent_path())
    throw Error(4, "preview staging directory must be beside the output image");
  return canonical;
}

constexpr auto preview_candidate = "candidate.dwarfs";

class PreviewLease {
public:
  PreviewLease(fs::path const &path, int mode)
      : fd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)) {
    struct stat info {};
    if (fd >= 0 && (::fstat(fd, &info) != 0 || info.st_uid != ::geteuid() ||
        (info.st_mode & 0777) != 0700 || !(info.st_mode & S_ISVTX) ||
        ::flock(fd, mode | LOCK_NB) != 0)) {
      ::close(fd);
      fd = -1;
    }
  }
  ~PreviewLease() { if (fd >= 0) ::close(fd); }
  PreviewLease(PreviewLease const &) = delete;
  PreviewLease &operator=(PreviewLease const &) = delete;
  int descriptor() const { return fd; }
private:
  int fd;
};

void reclaim_abandoned_previews(fs::path const &parent, fs::path const &current)
{
  std::error_code ec;
  for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
    cancellation_point();
    const auto path = it->path();
    if (path == current || !path.filename().string().starts_with(".dwarfsctl-preview-") ||
        path.filename().string().size() != sizeof(".dwarfsctl-preview-") - 1 + 6)
      continue;
    PreviewLease lease(path, LOCK_EX);
    const int fd = lease.descriptor();
    if (fd < 0) continue;

    // Walk the pinned directory and unlink only files created by the preview flow.
    const auto pinned = fs::path("/proc/self/fd") / std::to_string(fd);
    std::error_code scanError;
    bool unexpected = false;
    for (fs::directory_iterator items(pinned, scanError), done;
         !scanError && items != done; items.increment(scanError)) {
      const auto name = items->path().filename().string();
      const bool temporary = name.starts_with(".dwarfsctl-") && name.size() == 17;
      struct stat file {};
      if ((!temporary && name != preview_candidate) ||
          ::fstatat(fd, name.c_str(), &file, AT_SYMLINK_NOFOLLOW) != 0 ||
          !S_ISREG(file.st_mode) || file.st_uid != ::geteuid() ||
          ::unlinkat(fd, name.c_str(), 0) != 0)
        unexpected = true;
    }
    if (scanError || unexpected) continue;
    struct stat held {}, named {};
    if (::fstat(fd, &held) == 0 && ::lstat(path.c_str(), &named) == 0 &&
        held.st_dev == named.st_dev && held.st_ino == named.st_ino)
      ::rmdir(path.c_str());
  }
}

// The copy client owns this private sibling directory so a forced CLI stop
// cannot orphan data outside its cleanup scope. Pin it until publication ends.
class CopyStaging {
public:
  explicit CopyStaging(fs::path const &parent) : directory(parent) {
    const char *value = std::getenv("DWARFSCTL_COPY_STAGING_DIR");
    if (!value) return;
    const fs::path rawPath(value);
    if (!rawPath.is_absolute())
      throw Error(2, "copy staging must be a private, owned sibling directory");
    struct stat info {}, parentInfo {};
    std::error_code parentError, stagingError, pathError;
    const auto canonicalParent = fs::canonical(parent, parentError);
    const auto stagingParent = fs::canonical(rawPath.parent_path(), stagingError);
    const auto canonicalPath = fs::canonical(rawPath, pathError);
    if (parentError || stagingError || pathError || stagingParent != canonicalParent)
      throw Error(2, "copy staging must be a private, owned sibling directory");
    fd = ::open(canonicalPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 || ::fstat(fd, &info) != 0 || ::stat(parent.c_str(), &parentInfo) != 0 ||
        info.st_uid != ::geteuid() || (info.st_mode & 077) != 0 ||
        info.st_dev != parentInfo.st_dev) {
      if (fd >= 0) ::close(fd);
      fd = -1;
      throw Error(2, "copy staging must be a private, owned sibling directory");
    }
    directory = fs::path("/proc/self/fd") / std::to_string(fd);
  }
  ~CopyStaging() { if (fd >= 0) ::close(fd); }
  fs::path directory;
private:
  int fd = -1;
};

std::vector<std::string> extractionArguments(std::vector<std::string> const &raw) {
  const std::set<std::string> values{"--pattern", "--image-offset", "--num-workers", "--num-disk-writers", "--cache-size", "--log-level"};
  const std::set<std::string> switches{"--skip-devices", "--skip-specials", "--stdout-progress", "--log-with-context"};
  std::vector<std::string> result;
  bool patterns = false;
  for (size_t i = 0; i < raw.size(); ++i) {
    auto token = raw[i];
    if (!patterns && token == "--") { patterns = true; continue; }
    if (patterns || token.empty() || token[0] != '-') {
      result.push_back("--pattern=" + token);
      continue;
    }
    if (token.size() > 2 && token[2] != '=' && token[0] == '-' && token[1] != '-' &&
        (token[1] == 'O' || token[1] == 'n' || token[1] == 's'))
      token.insert(2, "=");
    const auto equal = token.find('=');
    auto option = token.substr(0, equal);
    std::string value = equal == std::string::npos ? "" : token.substr(equal + 1);
    if (option == "-O") option = "--image-offset";
    if (option == "-n") option = "--num-workers";
    if (option == "-s") option = "--cache-size";
    if (switches.contains(option) && equal == std::string::npos) {
      result.push_back(option);
    } else if (values.contains(option)) {
      if (equal == std::string::npos) {
        if (++i == raw.size()) throw Error(2, "missing extraction option value: " + option);
        value = raw[i];
      }
      if (option == "--num-disk-writers" && value != "0")
        throw Error(2, "extraction uses one secure filesystem writer (--num-disk-writers=0)");
      result.push_back(option + "=" + value);
    } else {
      throw Error(2, "unsupported extraction option: " + option + "; input, output, integrity and operation mode are owned by dwarfsctl");
    }
  }
  return result;
}

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
      throw Error(4, "destination exists; pass --force to overwrite", {{"reason", "destination_exists"}});
    if (inner.empty() || inner == "/")
      throw Error(4, "expected a file path inside the image");
    for (auto const &p : fs::path(inner))
      if (p == "." || p == "..")
        throw Error(4, "dot path components are not allowed");
    CopyStaging ownedStaging(dest.parent_path());
    Temp tmp(ownedStaging.directory);
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
      throw Error(4, "extraction destination already exists: " + dest.string(), {{"reason", "destination_exists"}});
    auto staging = std::make_unique<QTemporaryDir>(
        qs((dest.parent_path() / ".dwarfsctl-extract-XXXXXX").string()));
    if (!staging->isValid())
      throw Error(9, "cannot create extraction staging directory beside " +
                         dest.string());
    fs::path extract_dest = staging->path().toStdString();
    auto args = extractionArguments(in.many("arg"));
    Run r;
    auto paths = in.many("path");
    if (!paths.empty()) {
      std::sort(paths.begin(), paths.end());
      std::vector<std::string> selected;
      std::vector<std::string> requested;
      for (auto path : paths) {
        if (path.empty() || path.front() != '/' ||
            path.find('\0') != std::string::npos)
          throw Error(4, "selection must be an absolute image path");
        if (path.find("//") != std::string::npos)
          throw Error(4, "empty path components are not allowed");
        while (path.size() > 1 && path.back() == '/')
          path.pop_back();
        for (auto const &part : fs::path(path))
          if (part == "." || part == "..")
            throw Error(4, "dot path components are not allowed");
        requested.push_back(path);
        if (std::none_of(selected.begin(), selected.end(),
                         [&](auto const &root) {
                           return root == "/" || root == path ||
                                  path.starts_with(root + '/');
                         }))
          selected.push_back(std::move(path));
      }
      paths = std::move(selected);
      auto selected_args = requested;
      selected_args.insert(selected_args.begin(),
                           {image.string(), extract_dest.string()});
      r = engine("extract-selected", selected_args);
    } else if (args.empty())
      r = engine("extract", {image.string(), extract_dest.string()});
    else {
      args.insert(args.begin(),
                  {"-i", resolved_path(image), "-o", extract_dest.string()});
      r = engine("dwarfsextract", args);
    }
    require_success(r);
    cancellation_point();
    if (::syscall(SYS_renameat2, AT_FDCWD, extract_dest.c_str(), AT_FDCWD,
                  dest.c_str(), RENAME_NOREPLACE) != 0)
      throw Error(errno == EEXIST ? 4 : 9,
                  "cannot publish extraction directory " + dest.string() +
                      ": " + std::strerror(errno));
    J details = {{"dest", in.positions[1]}, {"tool", r.payload}};
    if (!paths.empty())
      details["selected_paths"] = paths;
    return details;
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
  if (in.command == "ls" && in.has("path")) {
    const auto mode = in.has("entry-only") ? "entry" : in.has("children-only") ? "children" : "subtree";
    auto result = engine("ls", {image.string(), in.get("path"), mode});
    require_success(result);
    auto entries = J::parse(result.out.toStdString());
    if (!entries.is_object() || !entries.contains("entries") || !entries["entries"].is_array())
      throw Error(9, "invalid image listing response");
    return entries;
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
  return {{"entries", entries}};
}
J build_command(Input const &in, Config const &cfg) {
  cancellation_point();
  fs::path source = in.positions[0];
  fs::path dest = output_path(in.get("output"));
  Lock lock(runtime_mount_root() / "locks" / (hash(dest.string()) + ".lock"));
  const bool preview = in.has("preview");
  bool overwrite =
      in.has("force") || in.has("overwrite-output") || in.has("yes");
  if (!preview && path_exists(dest) && !overwrite)
    throw Error(2, "output exists; pass --force or --overwrite-output");
  if (in.has("publish-preview")) {
    const auto candidate = fs::absolute(in.get("publish-preview")).lexically_normal();
    const auto stage = checked_preview_stage(candidate.parent_path(), dest);
    PreviewLease lease(stage, LOCK_SH);
    if (lease.descriptor() < 0 || candidate != stage / preview_candidate)
      throw Error(4, "invalid preview candidate path");
    struct stat info {};
    if (::lstat(candidate.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid())
      throw Error(4, "preview candidate is missing or changed");
    const auto raw = in.get("expected-size");
    uint64_t expected = 0;
    const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), expected);
    if (error != std::errc() || end != raw.data() + raw.size() || expected == 0)
      throw Error(2, "invalid expected preview size");
    if (fs::file_size(candidate) != expected)
      throw Error(4, "preview candidate size changed");
    std::cerr << J({{"event", "build_progress"}, {"phase", "verifying"}}).dump() << std::endl;
    const auto metadata = inspect(candidate);
    if (!metadata.contains("root")) throw Error(9, "preview candidate could not be verified");
    cancellation_point();
    publish(candidate, dest, overwrite);
    return {{"output_image", dest.string()}, {"published_preview", true},
            {"image_bytes", expected}, {"output_verified", true}};
  }
  if (!fs::is_directory(source))
    throw Error(4, "source must be an existing directory");
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
  fs::path stage;
  std::optional<PreviewLease> previewLease;
  if (in.has("preview-stage")) {
    stage = checked_preview_stage(in.get("preview-stage"), dest);
    previewLease.emplace(stage, LOCK_SH);
    if (previewLease->descriptor() < 0)
      throw Error(4, "preview staging directory has no active lease");
    reclaim_abandoned_previews(dest.parent_path(), stage);

    const auto relative = stage.lexically_relative(fs::path(actual[1]));
    if (!relative.empty() && *relative.begin() != "..") {
      // DwarFS uses the first matching rule. Exclude this exact private
      // directory before applying any filters from the compression profile.
      std::string rule = "--filter=- /";
      for (char c : relative.generic_string()) {
        if (std::string_view(R"(*?[]\)").contains(c)) rule += '\\';
        rule += c;
      }
      actual.insert(actual.begin() + 4, rule + '/');
    }
  }
  Temp temp(stage.empty() ? dest.parent_path() : stage);
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
  std::map<std::string, std::pair<uint64_t, uint64_t>> byType;
  std::function<void(J const &)> count = [&](J const &entry) {
    if (entry.value("type", "") == "file") {
      ++files;
      const auto bytes = entry.value("size", uint64_t(0));
      original += bytes;
      auto extension = fs::path(entry.value("name", "")).extension().string();
      if (extension.empty() || extension.size() > 16) extension = "(no extension)";
      else for (auto &character : extension)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
      auto &type = byType[extension];
      ++type.first;
      type.second += bytes;
    }
    if (entry.contains("inodes"))
      for (auto const &child : entry["inodes"])
        count(child);
  };
  count(metadata.at("root"));
  std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> types(byType.begin(), byType.end());
  std::sort(types.begin(), types.end(), [](auto const &left, auto const &right) {
    return left.second.second > right.second.second;
  });
  J breakdown = J::array();
  uint64_t remainingFiles = 0, remainingBytes = 0;
  for (size_t index = 0; index < types.size(); ++index) {
    auto const &[name, totals] = types[index];
    if (index < 5) breakdown.push_back({{"type", name}, {"files", totals.first}, {"original_bytes", totals.second}});
    else { remainingFiles += totals.first; remainingBytes += totals.second; }
  }
  if (remainingFiles) breakdown.push_back({{"type", "Other types"}, {"files", remainingFiles}, {"original_bytes", remainingBytes}});
  details["summary"] = {
      {"files", files},
      {"original_bytes", original},
      {"image_bytes", fs::file_size(temp.path)},
      {"preview", preview},
      {"source_types", breakdown},
      {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - started)
                         .count()}};
  if (!preview) publish(temp.path, dest, overwrite);
  else {
    details["output_verified"] = false;
    details["preview_verified"] = true;
    if (!stage.empty()) {
      const auto candidate = stage / preview_candidate;
      if (::rename(temp.path.c_str(), candidate.c_str()) != 0)
        throw Error(9, "could not retain measured preview: " + std::string(std::strerror(errno)));
      details["preview_candidate"] = candidate.string();
    }
  }
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
