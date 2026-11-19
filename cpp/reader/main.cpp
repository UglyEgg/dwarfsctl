// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Richard Majewski
#include <dwarfs/block_compressor.h>
#include <dwarfs/glob_matcher.h>
#include <dwarfs/logger.h>
#include <dwarfs/os_access_generic.h>
#include <dwarfs/reader/filesystem_v2.h>
#include <dwarfs/reader/fsinfo_options.h>
#include <dwarfs/tool/main_adapter.h>
#include <dwarfs/utility/filesystem_extractor.h>
#include <dwarfs/version.h>
#include <dwarfs_tool_main.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// Private, positional protocol. stdout is JSON for inspect/ls/version and raw
// bytes for cat; all diagnostics go to stderr. The public C++ CLI owns argument
// validation, stable envelopes, locking and atomic destination publication.
int main(int argc, char **argv) {
  try {
    if (argc >= 2) {
      using namespace dwarfs::tool;
      std::string_view op(argv[1]);
      if (op == "validate-compression" && argc == 3) {
        dwarfs::block_compressor compressor(argv[2]);
        return 0;
      }
      if (op == "mkdwarfs")
        return main_adapter(mkdwarfs_main).safe(argc - 1, argv + 1);
      if (op == "dwarfsck")
        return main_adapter(dwarfsck_main).safe(argc - 1, argv + 1);
      if (op == "dwarfsextract")
        return main_adapter(dwarfsextract_main).safe(argc - 1, argv + 1);
      if (op == "dwarfs")
        return main_adapter(dwarfs_main).safe(argc - 1, argv + 1);
    }
    if (argc == 2 && std::string_view(argv[1]) == "version") {
      std::cout << "{\"protocol\":1,\"dwarfs\":\"" << DWARFS_VERSION_MAJOR
                << '.' << DWARFS_VERSION_MINOR << '.' << DWARFS_VERSION_PATCH
                << "\"}\n";
      return 0;
    }
    if (argc < 3)
      throw std::invalid_argument(
          "expected inspect, cat or extract and an image");
    std::string_view command(argv[1]);
    if ((command == "inspect" && argc != 3) ||
        ((command == "cat" || command == "extract") && argc != 4) ||
        (command == "ls" && argc != 5) ||
        (command == "extract-selected" && argc < 5) ||
        (command != "inspect" && command != "cat" && command != "extract" &&
         command != "extract-selected" && command != "ls"))
      throw std::invalid_argument("invalid reader protocol arguments");
    dwarfs::os_access_generic os;
    dwarfs::stream_logger logger(std::cerr, os);
    dwarfs::reader::filesystem_v2 fs(logger, os, argv[2]);
    if (command == "inspect") {
      std::cout << fs.info_as_json(
                         {.features = dwarfs::reader::fsinfo_features::parse(
                              "metadata_summary,directory_tree")})
                       .dump()
                << '\n';
    } else if (command == "ls") {
      // Select before serializing metadata: a narrow query must not expand all
      // unrelated directories. Lookup is literal and never traverses symlinks.
      std::string path(argv[3]);
      std::string_view mode(argv[4]);
      if (path.empty() || path.front() != '/' ||
          path.find("//") != std::string::npos ||
          (mode != "entry" && mode != "children" && mode != "subtree"))
        throw std::invalid_argument("invalid image listing query");
      while (path.size() > 1 && path.back() == '/') path.pop_back();
      for (auto const &component : std::filesystem::path(path))
        if (component == "." || component == "..")
          throw std::invalid_argument("dot components are not allowed");
      auto found = fs.find(path);
      if (!found)
        throw std::invalid_argument("no such path in image: " + path);
      auto stored = found->unix_path();
      if (stored.empty()) stored = "/";
      else if (stored.front() != '/') stored = "/" + stored;
      if (stored != path)
        throw std::invalid_argument("no such literal path in image: " + path);
      if (mode == "children" && !found->inode().is_directory())
        throw std::invalid_argument("not a directory in image: " + path);
      nlohmann::json entries = nlohmann::json::array();
      auto append = [&](auto const &entry, std::string const &entryPath) {
        auto inode = entry.inode();
        entries.push_back({{"name", entryPath == "/" ? "" : entry.name()},
                           {"path", entryPath},
                           {"is_dir", inode.is_directory()},
                           {"mode", inode.mode()},
                           {"size", inode.is_regular_file()
                                        ? nlohmann::json(fs.getattr(inode).size())
                                        : nlohmann::json(nullptr)}});
      };
      std::function<void(dwarfs::reader::dir_entry_view const &,
                         std::string const &, bool)> children;
      children = [&](auto const &entry, std::string const &parent, bool recursive) {
        auto directory = fs.opendir(entry.inode());
        if (!directory) return;
        // Offsets zero and one are synthetic dot entries, not stored children.
        for (size_t offset = 2; auto child = fs.readdir(*directory, offset); ++offset) {
          auto name = child->name();
          if (name.empty() || name == "." || name == ".." ||
              name.find('/') != std::string::npos || name.find('\0') != std::string::npos)
            throw std::invalid_argument("unsafe entry name in image");
          auto childPath = parent + (parent == "/" ? "" : "/") + name;
          append(*child, childPath);
          if (recursive && child->inode().is_directory()) children(*child, childPath, true);
        }
      };
      if (mode != "children") append(*found, path);
      if (mode != "entry") children(*found, path, mode == "subtree");
      std::sort(entries.begin(), entries.end(), [](auto const &a, auto const &b) {
        return a["path"] < b["path"];
      });
      std::cout << nlohmann::json({{"entries", std::move(entries)}}).dump() << '\n';
    } else if (command == "cat") {
      // Exact path lookup, never a glob; brackets and asterisks are filenames.
      std::string path(argv[3]);
      if (path.empty() || path == "/")
        throw std::invalid_argument("expected a file path");
      for (auto const &component : std::filesystem::path(path))
        if (component == "." || component == "..")
          throw std::invalid_argument("dot components are not allowed");
      auto entry = fs.find(path);
      if (!entry || !entry->inode().is_regular_file())
        throw std::invalid_argument("no regular file at requested image path");
      auto inode = entry->inode();
      auto size = fs.getattr(inode).size();
      std::array<char, 128 * 1024> buffer;
      dwarfs::file_off_t offset = 0;
      while (offset < size) {
        auto n =
            fs.read(inode.inode_num(), buffer.data(),
                    std::min<uint64_t>(buffer.size(), size - offset), offset);
        if (n == 0)
          throw std::runtime_error("unexpected end of image file");
        std::cout.write(buffer.data(), static_cast<std::streamsize>(n));
        if (!std::cout)
          throw std::runtime_error("could not write file data");
        offset += static_cast<dwarfs::file_off_t>(n);
      }
    } else {
      // Single writer keeps libarchive's secure-symlink checks enabled. The
      // extractor changes cwd, isolated inside this short-lived helper process.
      dwarfs::glob_matcher matcher;
      bool const selected = command == "extract-selected";
      if (selected) {
        // Resolve literal paths against stored entries, never through symlinks.
        std::map<std::string, std::pair<std::string, bool>> entries;
        fs.walk([&](auto entry) {
          auto raw = entry.unix_path();
          auto path = raw.empty() || raw == "/" ? "/"
                      : raw.front() == '/'      ? raw
                                                : "/" + raw;
          entries.emplace(path, std::pair{raw, entry.inode().is_directory()});
        });
        std::vector<std::string> roots;
        auto escape = [](std::string const &path) {
          std::string pattern;
          for (char c : path) {
            if (std::string_view(R"(\*?[]{})").find(c) !=
                std::string_view::npos)
              pattern += '\\';
            pattern += c;
          }
          return pattern;
        };
        for (int i = 4; i < argc; ++i) {
          std::string path(argv[i]);
          if (path.empty() || path.front() != '/' ||
              path.find("//") != std::string::npos)
            throw std::invalid_argument(
                "selection must be an absolute image path");
          while (path.size() > 1 && path.back() == '/')
            path.pop_back();
          for (auto const &part : std::filesystem::path(path))
            if (part == "." || part == "..")
              throw std::invalid_argument("dot components are not allowed");
          auto found = entries.find(path);
          if (found == entries.end())
            throw std::invalid_argument("no such path in image: " + path);
          roots.push_back(path);
          auto const &[raw, directory] = found->second;
          // Explicit options prevent interpretation of leading i: or :
          // prefixes.
          matcher.add_pattern(escape(raw), dwarfs::glob_matcher::options{});
          if (directory)
            matcher.add_pattern(path == "/" ? "**" : escape(raw) + "/**",
                                dwarfs::glob_matcher::options{});
        }
        auto wanted = [&](std::string const &path) {
          return std::any_of(roots.begin(), roots.end(), [&](auto const &root) {
            return root == "/" || path == root || path.starts_with(root + '/');
          });
        };
        // Upstream ** uses a regex dot, which excludes line breaks. Supplement
        // those entries with escaped exact patterns so all legal names work.
        for (auto const &[path, value] : entries)
          if (wanted(path) && !matcher.match(value.first))
            matcher.add_pattern(escape(value.first),
                                dwarfs::glob_matcher::options{});
        // Fail closed if any glob translation differs from literal selection.
        // This metadata-only pass completes before the extractor reads data.
        for (auto const &[path, value] : entries) {
          if (matcher.match(value.first) != wanted(path))
            throw std::runtime_error("literal selection matcher mismatch");
        }
      }
      dwarfs::utility::filesystem_extractor extractor(logger, os);
      extractor.open_disk(argv[3], 0);
      dwarfs::utility::filesystem_extractor_options options;
      options.include_matching_directories = selected;
      if (!extractor.extract(fs, selected ? &matcher : nullptr, options))
        throw std::runtime_error("image extraction failed");
      extractor.close();
    }
    std::cout.flush();
    if (!std::cout)
      throw std::runtime_error("could not flush output");
    return 0;
  } catch (std::invalid_argument const &e) {
    std::cerr << e.what() << '\n';
    return 4;
  } catch (std::exception const &e) {
    std::cerr << e.what() << '\n';
    return 9;
  }
}
