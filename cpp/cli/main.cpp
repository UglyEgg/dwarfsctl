// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include <QCoreApplication>
#include <algorithm>
#include <iostream>
using namespace ctl;
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("dwarfsctl");
  QCoreApplication::setApplicationVersion(CTL_VERSION);
  auto raw = app.arguments();
  raw.removeFirst();
  // Help/version are the only human-readable outputs, as with the previous CLI.
  bool pass_value = false;
  for (auto const &token : raw) {
    if (pass_value) {
      pass_value = false;
      continue;
    }
    if (token == "--")
      break;
    if (token == "--config" || token == "--output" || token == "--path" ||
        token == "--compression" || token == "--compress" ||
        token == "--section" || token == "--goal" || token == "--cache" ||
        token == "--algo" || token == "--input" || token == "--dir" ||
        token == "--image" || token == "--mountpoint" || token == "--arg" ||
        token == "--mkdwarfs-arg") {
      pass_value = true;
      continue;
    }
    if (token == "--version" || token == "-V") {
      std::cout << "dwarfsctl " CTL_VERSION
                   "\nUnofficial KDE integration for DwarFS.\nNot "
                   "affiliated with or endorsed by the DwarFS project.\n";
      return 0;
    }
    if (token == "--help" || token == "-h") {
      std::cout
          << "dwarfsctl " CTL_VERSION
             " — C++ KDE integration backend\nUsage: dwarfsctl "
             "[--config FILE] [--verbose] COMMAND [OPTIONS]\n\nCommands:\n";
      for (auto const &c : command_paths())
        std::cout << "  " << c << '\n';
      std::cout << "\nSee dwarfsctl(1) for flags and arguments. Results use "
                   "JSON envelopes.\n";
      return 0;
    }
  }
  std::string command = "bootstrap";
  J image = nullptr, warnings = J::array();
  try {
    auto in = parse(raw);
    command = in.command;
    std::replace(command.begin(), command.end(), ' ', '_');
    std::replace(command.begin(), command.end(), '-', '_');
    if (!in.positions.empty() && !in.command.starts_with("config") &&
        !in.command.starts_with("mkdwarfs") && in.command != "build")
      image = in.positions[0];
    Config config(in.get("config"));
    warnings = config.warnings;
    J result;
    if (in.command.starts_with("mount") || in.command.starts_with("unmount")) {
      result = mount_command(in);
      if (in.command != "mount registry-path") {
        if (!result.contains("error") || !result["ok"].is_boolean())
          throw Error(9, "invalid mount response");
        int code = result["ok"] == true ? 0 : result["error"].value("code", 9);
        std::cout << result.dump() << '\n';
        return code;
      }
    } else if (in.command == "config show") {
      result = config.value;
      if (in.has("section")) {
        auto section = in.get("section");
        if (!result.contains(section))
          throw Error(2, "unknown config section: " + section);
        result = result.at(section);
      }
    } else if (in.command == "config set") {
      result = config_set_command(in);
    } else if (in.command.starts_with("config mkdwarfs-profiles"))
      result = profile_command(in, config);
    else if (in.command == "mkdwarfs recommend")
      result = recommend(in);
    else if (in.command == "mkdwarfs schema") {
      result = {{"supported_mkdwarfs_version",
                 schema()["supported_mkdwarfs_version"]},
                {"installed_mkdwarfs_version", "0.15.7"},
                {"schema", schema()}};
      if (in.has("include-long-help")) {
        auto r = engine("mkdwarfs", {"--long-help"});
        require_success(r);
        result["mkdwarfs_long_help"] = str(QString::fromUtf8(r.out));
      }
    } else if (in.command == "tools schema" || in.command == "doctor" ||
               in.command == "status")
      result = diagnostics(in, config);
    else if (in.command == "build") {
      result = build_command(in, config);
      image = result["output_image"];
    } else if (in.command.starts_with("checksum"))
      result = checksum_command(in, config);
    else
      result = read_command(in, config);
    std::cout << envelope(command, image, result, warnings).dump() << '\n';
    return 0;
  } catch (Error const &e) {
    std::cout << envelope(command, image, e.details, warnings, &e).dump()
              << '\n';
    return e.code;
  } catch (std::exception const &e) {
    Error error(9, e.what());
    std::cout << envelope(command, image, nullptr, warnings, &error).dump()
              << '\n';
    return 9;
  }
}
