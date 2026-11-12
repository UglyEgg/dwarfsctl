// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include <algorithm>
#include <fstream>
#include <set>
namespace ctl {
static uint64_t number(J const &p, char const *key) {
  return p[key].get<uint64_t>();
}
J recommend(Input const &in) {
  auto goal = in.get("goal", "balanced"), cache = in.get("cache", "on");
  if (goal != "smallest" && goal != "balanced" && goal != "fastest-browse" &&
      goal != "fast-build")
    throw Error(2, "unknown recommendation goal");
  if (cache != "on" && cache != "off")
    throw Error(2, "cache must be on or off");
  fs::path root = in.positions[0];
  std::error_code root_error;
  auto st = fs::symlink_status(root, root_error);
  if (root_error || !fs::is_directory(st) || fs::is_symlink(st))
    throw Error(4, "source must be a directory, not a symlink");
  J p = {{"files", 0u},
         {"directories", 0u},
         {"total_bytes", 0u},
         {"sampled_files", 0u},
         {"sampled_bytes", 0u},
         {"precompressed_sample_bytes", 0u},
         {"skipped_symlinks", 0u},
         {"skipped_special", 0u},
         {"unreadable", 0u},
         {"truncated", false}};
  auto add = [&](char const *key, uint64_t n = 1) {
    auto old = number(p, key);
    p[key] = UINT64_MAX - old < n ? UINT64_MAX : old + n;
  };
  const std::set<std::string> compressed{
      "zip",    "gz",   "xz",   "zst", "7z",   "rar",  "bz2",
      "dwarfs", "jpg",  "jpeg", "png", "webp", "avif", "mp3",
      "ogg",    "flac", "mp4",  "mkv", "webm", "pdf"};
  size_t budget = 20000;
  std::function<void(fs::path const &, size_t)> walk = [&](auto const &dir,
                                                           size_t depth) {
    if (depth > 32) {
      p["truncated"] = true;
      return;
    }
    std::error_code ec;
    fs::directory_iterator it(dir, ec), end;
    if (ec) {
      if (!depth)
        throw Error(4, "cannot read source directory");
      add("unreadable");
      return;
    }
    std::vector<fs::path> paths;
    for (; it != end; it.increment(ec)) {
      if (ec) {
        add("unreadable");
        break;
      }
      if (!budget) {
        p["truncated"] = true;
        return;
      }
      --budget;
      paths.push_back(it->path());
    }
    std::sort(paths.begin(), paths.end());
    for (auto const &path : paths) {
      auto type = fs::symlink_status(path, ec);
      if (ec) {
        add("unreadable");
        continue;
      }
      if (fs::is_symlink(type))
        add("skipped_symlinks");
      else if (fs::is_directory(type)) {
        add("directories");
        walk(path, depth + 1);
      } else if (fs::is_regular_file(type)) {
        auto size = fs::file_size(path, ec);
        if (ec) {
          add("unreadable");
          continue;
        }
        add("files");
        add("total_bytes", size);
        if (number(p, "sampled_files") < 512) {
          add("sampled_files");
          add("sampled_bytes", size);
          auto ext = path.extension().string();
          if (!ext.empty())
            ext.erase(0, 1);
          std::transform(ext.begin(), ext.end(), ext.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          if (compressed.contains(ext))
            add("precompressed_sample_bytes", size);
        }
      } else
        add("skipped_special");
    }
  };
  walk(root, 0);
  int level, bits;
  std::string compression, reason;
  if (goal == "smallest") {
    level = 9;
    bits = 24;
    compression = "zstd:level=19";
    reason = "Prioritize compression ratio; building and random reads may cost "
             "more.";
  } else if (goal == "fastest-browse") {
    level = 3;
    bits = 18;
    compression = "lz4";
    reason = "Use fast decompression and smaller blocks for random reads; "
             "images may be larger.";
  } else if (goal == "fast-build") {
    level = 1;
    bits = 20;
    compression = "zstd:level=1";
    reason = "Use a low-cost preset and compressor to reduce build work.";
  } else {
    level = 7;
    bits = 22;
    compression = "zstd:level=6";
    reason = "Balance image size, build cost and browsing latency.";
  }
  J reasons = J::array({reason});
  if (goal == "balanced" && number(p, "files") &&
      number(p, "total_bytes") / number(p, "files") < 16384) {
    bits = 20;
    reasons.push_back("Observed files average less than 16 KiB; use smaller "
                      "blocks for browsing.");
  }
  if (goal == "balanced" && number(p, "sampled_bytes") &&
      static_cast<long double>(number(p, "precompressed_sample_bytes")) /
              number(p, "sampled_bytes") >=
          0.7L) {
    compression = "zstd:level=1";
    reasons.push_back("At least 70% of sampled bytes have commonly compressed "
                      "extensions; avoid expensive recompression.");
  }
  J warnings = J::array({"Metadata heuristic only: extensions do not prove "
                         "compressibility; no benchmark was run."});
  if (p["truncated"] == true)
    warnings.push_back("Scan limit reached; counts and recommendations cover "
                       "only the inspected portion.");
  if (number(p, "unreadable"))
    warnings.push_back("Some entries could not be inspected.");
  if (number(p, "skipped_symlinks"))
    warnings.push_back(
        "Symbolic links were skipped; their targets were not inspected.");
  if (number(p, "skipped_special"))
    warnings.push_back("Special files were skipped.");
  if (!number(p, "files"))
    warnings.push_back(
        "No regular files were observed; using the selected goal's defaults.");
  if (number(p, "files") > number(p, "sampled_files"))
    warnings.push_back("Extension sampling used the first 512 regular files in "
                       "sorted depth-first order.");
  auto confidence = p["truncated"] == true || number(p, "unreadable") ||
                            number(p, "files") < 10 ||
                            number(p, "files") > number(p, "sampled_files")
                        ? "low"
                        : "medium";
  std::vector<std::string> flags{"--compress-level=" + std::to_string(level),
                                 "--compression=" + compression,
                                 "--block-size-bits=" + std::to_string(bits)};
  validate_profile(flags);
  J rec = {{"flags_only", flags},
           {"reasons", reasons},
           {"warnings", warnings},
           {"confidence", confidence}};
  J result = {{"schema", "dwarfsctl.recommend.v1"},
              {"goal", goal},
              {"profile", p},
              {"recommendation", rec},
              {"cache_hit", false}};
  if (cache == "off")
    return result;
  fs::path base;
  auto xdg = std::getenv("XDG_CACHE_HOME");
  auto home = std::getenv("HOME");
  if (xdg && fs::path(xdg).is_absolute())
    base = xdg;
  else if (home)
    base = fs::path(home) / ".cache";
  else
    return result;
  auto path =
      base / "dwarfsctl/recommend" / (hash(resolved_path(root)) + ".json");
  std::error_code ec;
  auto size = fs::file_size(path, ec);
  if (!ec && size < 65536) {
    try {
      std::ifstream file(path);
      J old;
      file >> old;
      result["cache_hit"] =
          old.value("schema", "") == result.value("schema", "") &&
          old.value("goal", "") == goal && old.value("profile", J()) == p &&
          old.value("recommendation", J()) == rec;
    } catch (std::exception const &) {
    }
  }
  if (result["cache_hit"] == false)
    try {
      atomic_write(path, result.dump());
    } catch (std::exception const &) {
      result["recommendation"]["warnings"].push_back(
          "Recommendation cache could not be written; result is still usable.");
    }
  return result;
}
} // namespace ctl
