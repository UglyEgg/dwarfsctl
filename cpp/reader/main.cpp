// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Richard Majewski
#include <dwarfs/block_compressor.h>
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
#include <stdexcept>
#include <string>

// Private, positional protocol. stdout is JSON for inspect/version and raw
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
        (command != "inspect" && command != "cat" && command != "extract"))
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
      // Validate every stored component before libarchive sees a pathname.
      // Symlink targets may be external, but no archive entry may write there.
      fs.walk([](dwarfs::reader::dir_entry_view entry) {
        if (entry.is_root())
          return;
        auto name = entry.name();
        if (name.empty() || name == "." || name == ".." ||
            name.find('/') != std::string::npos ||
            name.find('\0') != std::string::npos)
          throw std::invalid_argument("unsafe entry name in image");
      });
      dwarfs::utility::filesystem_extractor extractor(logger, os);
      extractor.open_disk(argv[3], 0);
      if (!extractor.extract(fs))
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
