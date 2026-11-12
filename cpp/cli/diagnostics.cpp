// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QRegularExpression>
#include <QStandardPaths>
#include <fstream>
#include <sstream>
#include <unistd.h>
namespace ctl {
J mount_command(Input const &in) {
  if (in.command == "mount registry-path") {
    auto run =
        str(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation));
    if (run.empty())
      run = "/run/user/" + std::to_string(getuid());
    return {{"path", run + "/dwarfsctl/mounts.json"}};
  }
  std::string method;
  QList<QVariant> args;
  if (in.command == "mount") {
    method = "Mount";
    args << qs(resolved_path(in.positions[0]));
  } else if (in.command == "mount list")
    method = "ListMounts";
  else if (in.command == "mount status") {
    method = "Status";
    args << qs(resolved_path(in.positions[0]));
  } else if (in.command == "unmount all")
    method = "UnmountAll";
  else if (in.command == "unmount last")
    method = "UnmountLast";
  else if (in.has("mountpoint")) {
    method = "UnmountByMountpoint";
    args << qs(resolved_path(in.get("mountpoint")));
  } else if (in.has("image")) {
    method = "UnmountByImage";
    args << qs(resolved_path(in.get("image")));
  } else {
    method = fs::is_directory(in.positions[0]) ? "UnmountByMountpoint"
                                               : "UnmountByImage";
    args << qs(resolved_path(in.positions[0]));
  }
  auto bus = QDBusConnection::sessionBus();
  if (!bus.isConnected())
    throw Error(3, "session D-Bus is unavailable");
  auto msg = QDBusMessage::createMethodCall(
      "org.quest.entropy.dwarfsctl.Tray", "/org/quest/entropy/dwarfsctl/Mounts",
      "org.quest.entropy.dwarfsctl.Mounts1", qs(method));
  msg.setArguments(args);
  auto reply = bus.call(msg, QDBus::Block, 300000);
  if (reply.type() == QDBusMessage::ErrorMessage) {
    auto name = str(reply.errorName());
    int code = name.find("AccessDenied") != std::string::npos ? 6 : 9;
    if (name.find("ServiceUnknown") != std::string::npos ||
        name.find("NameHasNoOwner") != std::string::npos ||
        name.find("Unknown") != std::string::npos)
      code = 3;
    throw Error(code,
                "mount agent: " + name + ": " + str(reply.errorMessage()));
  }
  if (reply.arguments().size() != 1)
    throw Error(9, "invalid mount-agent reply");
  auto doc = J::parse(str(reply.arguments()[0].toString()));
  if (!doc.is_object() || !doc.contains("ok"))
    throw Error(9, "invalid mount-agent envelope");
  return doc;
}
J diagnostics(Input const &in, Config const &) {
  bool ready = false;
  std::string failure;
  try {
    auto result = engine("version", {});
    require_success(result);
    auto version = J::parse(result.out.toStdString());
    ready = version["dwarfs"] == "0.15.7" && version["protocol"] == 1;
    if (!ready)
      failure = "engine version mismatch";
  } catch (std::exception const &e) {
    failure = e.what();
  }
  J deps = J::object(), tools = J::array();
  for (auto const &key : {"mkdwarfs", "dwarfs", "dwarfsck", "dwarfsextract"}) {
    deps[key] = {{"name", key}, {"found", ready}, {"path", engine_path()}};
    tools.push_back({{"key", key},
                     {"found", ready},
                     {"path", engine_path()},
                     {"installed_version", ready ? J("0.15.7") : J(nullptr)},
                     {"supported_min", "0.15.7"},
                     {"supported_max_tested", "0.15.7"}});
  }
  if (in.command == "tools schema")
    return {
        {"schema_version", 1},
        {"tools", tools},
        {"supported_mkdwarfs_version", schema()["supported_mkdwarfs_version"]},
        {"installed_mkdwarfs_version", ready ? J("0.15.7") : J(nullptr)}};
  if (in.command == "doctor") {
    J checks = J::array();
    for (auto const &key : {"mkdwarfs", "dwarfs", "dwarfsck", "dwarfsextract"})
      checks.push_back({{"id", std::string("tool_") + key},
                        {"ok", ready},
                        {"severity", ready ? "info" : "error"},
                        {"summary", ready ? std::string(key) +
                                                " supplied by pinned C++ engine"
                                          : failure}});
    checks.push_back(
        {{"id", "native_reader"},
         {"ok", ready},
         {"severity", ready ? "info" : "error"},
         {"summary",
          ready ? "C++ engine with embedded DwarFS 0.15.7" : failure}});
    checks.push_back(
        {{"id", "fuse_device"},
         {"ok", ::access("/dev/fuse", R_OK | W_OK) == 0},
         {"severity", "warning"},
         {"summary", "FUSE device access is needed only for mounting"}});
    checks.push_back(
        {{"id", "session_bus"},
         {"ok", QDBusConnection::sessionBus().isConnected()},
         {"severity", "warning"},
         {"summary", "session bus is needed only for mount control"}});
    return {{"checks", checks}};
  }
  if (!ready)
    throw Error(3, failure);
  J details = {{"deps", deps}};
  if (in.positions.empty())
    return details;
  fs::path image = in.positions[0];
  if (!fs::is_regular_file(image))
    throw Error(4, "image not found");
  auto canonical = resolved_path(image), digest = hash(canonical);
  auto stem = str(qs(fs::path(canonical).stem().string())
                      .replace(QRegularExpression("[^A-Za-z0-9_.-]"), "_"));
  if (stem.empty())
    stem = "dwarfs";
  auto mount_root = runtime_mount_root();
  auto mp = (mount_root / (stem + "-" + digest.substr(0, 8))).string();
  auto lockdir = (mount_root / "locks").string();
  Lock lock(fs::path(lockdir) / (digest + ".lock"));
  details["image"] = {
      {"path", image.string()},
      {"exists", true},
      {"paths",
       {{"canonical", canonical},
        {"hash", digest},
        {"mount_root", mount_root.string()},
        {"mountpoint", mp},
        {"lock_dir", lockdir},
        {"lock_path", (fs::path(lockdir) / (digest + ".lock")).string()}}}};
  details["mountpoint"] = mp;
  details["mounted"] = false;
  std::ifstream mounts("/proc/self/mountinfo");
  std::string line;
  auto unescape = [](std::string value) {
    for (auto [from, to] :
         std::vector<std::pair<std::string, std::string>>{{"\\040", " "},
                                                          {"\\011", "\t"},
                                                          {"\\012", "\n"},
                                                          {"\\134", "\\"}}) {
      size_t p = 0;
      while ((p = value.find(from, p)) != std::string::npos) {
        value.replace(p, from.size(), to);
        p += to.size();
      }
    }
    return value;
  };
  while (std::getline(mounts, line)) {
    std::istringstream stream(line);
    std::vector<std::string> fields;
    std::string word;
    while (stream >> word)
      fields.push_back(word);
    if (fields.size() > 6 && unescape(fields[4]) == mp) {
      auto sep = std::find(fields.begin(), fields.end(), "-");
      details["mounted"] = true;
      if (sep != fields.end() && std::distance(sep, fields.end()) >= 3) {
        details["mount_fs_type"] = *(sep + 1);
        details["mount_source"] = unescape(*(sep + 2));
      }
      break;
    }
  }
  auto r = engine("dwarfsck",
                  {"--no-check", "--detail=7", "--json", "-i", image.string()});
  require_success(r);
  auto data = J::parse(r.out.toStdString());
  r.payload["json"] = data;
  details["dwarfsck"] = {{"detail", 7}, {"json", data}};
  details["tools"] = {{"dwarfsck", r.payload}};
  return details;
}
} // namespace ctl
