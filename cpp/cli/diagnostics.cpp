// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QStandardPaths>
#include <unistd.h>
namespace ctl {
namespace {
J queryMountAgent(QString const &method, QList<QVariant> const &args,
                  int timeoutMs) {
  auto bus = QDBusConnection::sessionBus();
  if (!bus.isConnected())
    throw Error(3, "session D-Bus is unavailable",
                {{"reason", "authority_unavailable"}, {"outcome", "not_dispatched"}});
  auto msg = QDBusMessage::createMethodCall(
      "org.quest.entropy.dwarfsctl.Tray", "/org/quest/entropy/dwarfsctl/Mounts",
      "org.quest.entropy.dwarfsctl.Mounts1", method);
  msg.setArguments(args);
  const auto reply = bus.call(msg, QDBus::Block, timeoutMs);
  if (reply.type() == QDBusMessage::ErrorMessage) {
    const auto name = str(reply.errorName());
    const bool uncertain = reply.errorName() == "org.freedesktop.DBus.Error.NoReply" ||
                           reply.errorName() == "org.freedesktop.DBus.Error.Timeout" ||
                           reply.errorName() == "org.freedesktop.DBus.Error.Disconnected";
    int code = name.find("AccessDenied") != std::string::npos ? 6 : 9;
    if (name.find("ServiceUnknown") != std::string::npos ||
        name.find("NameHasNoOwner") != std::string::npos ||
        name.find("Unknown") != std::string::npos) code = 3;
    auto message = "mount agent: " + name + ": " + str(reply.errorMessage());
    if (uncertain)
      message += ". The request may still complete. Check mount state before retrying.";
    throw Error(code, message,
                {{"reason", uncertain ? "outcome_unconfirmed" : "authority_unavailable"},
                 {"outcome", uncertain ? "unknown" : "not_dispatched"},
                 {"bus_error", name}, {"method", str(method)}});
  }
  if (reply.arguments().size() != 1)
    throw Error(9, "invalid mount-agent reply");
  auto doc = J::parse(str(reply.arguments()[0].toString()));
  if (!doc.is_object() || !doc.contains("ok") || !doc["ok"].is_boolean())
    throw Error(9, "invalid mount-agent envelope");
  return doc;
}
} // namespace
J mount_command(Input const &in) {
  if (in.command == "mount registry-path") {
    auto run =
        str(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation));
    if (run.empty())
      run = "/run/user/" + std::to_string(getuid());
    return {{"path", run + "/dwarfsctl/mounts.json"}};
  }
  // Recovery selectors must reach the authority even when their filesystem
  // cannot answer metadata requests. Physical alias resolution belongs there.
  const auto selector = [](const std::string &path) { return fs::absolute(path).string(); };
  std::string method;
  QList<QVariant> args;
  if (in.command == "mount") {
    method = "Mount";
    args << qs(selector(in.positions[0]));
  } else if (in.command == "mount list")
    method = "ListMounts";
  else if (in.command == "mount status") {
    method = "Status";
    args << qs(selector(in.positions[0]));
  } else if (in.command == "unmount all")
    method = "UnmountAll";
  else if (in.command == "unmount last")
    method = "UnmountLast";
  else if (in.has("mountpoint")) {
    method = "UnmountByMountpoint";
    args << qs(selector(in.get("mountpoint")));
  } else if (in.has("image")) {
    method = "UnmountByImage";
    args << qs(selector(in.get("image")));
  } else {
    // Ask the authority to select the recorded point first. Only its explicit
    // no-record rejection permits trying the image selector; no filesystem
    // classification or second local unmount authority is involved.
    args << qs(selector(in.positions[0]));
    const auto result = queryMountAgent(QStringLiteral("UnmountByMountpoint"), args, 300000);
    if (!result.value("ok", false) && result.contains("details") && result["details"].is_object()
        && result["details"].value("reason", std::string()) == "record_not_found")
      return queryMountAgent(QStringLiteral("UnmountByImage"), args, 300000);
    return result;
  }
  return queryMountAgent(qs(method), args, 300000);
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
  auto mount_root = runtime_mount_root();
  auto lockdir = (mount_root / "locks").string();
  details["image"] = {
      {"path", image.string()}, {"exists", true},
      {"paths", {{"canonical", canonical}, {"hash", digest},
                 {"mount_root", mount_root.string()}, {"mountpoint", nullptr},
                 {"lock_dir", lockdir},
                 {"lock_path", (fs::path(lockdir) / (digest + ".lock")).string()}}}};
  // Image inspection works offline. Mount knowledge belongs to the session
  // authority; an unavailable snapshot is different from a known empty one.
  details["mount_state_known"] = false;
  details["mountpoint"] = nullptr;
  try {
    const auto response = queryMountAgent(QStringLiteral("Status"), {qs(canonical)}, 3000);
    const auto state = response.value("details", J(nullptr));
    bool valid = response.value("ok", false) && state.is_object() &&
                 state.contains("mounted") && state["mounted"].is_boolean();
    if (valid) {
      for (const auto *key : {"mountpoint", "mount_health", "mount_fs_type", "mount_source"})
        if (state.contains(key) && !state[key].is_null() && !state[key].is_string()) valid = false;
      for (const auto *key : {"source_available", "generation_verified", "source_changed"})
        if (state.contains(key) && !state[key].is_null() && !state[key].is_boolean()) valid = false;
      if (state["mounted"] == true &&
          (!state.contains("mountpoint") || !state["mountpoint"].is_string() || state["mountpoint"] == "")) valid = false;
    }
    if (valid) {
      details["mount_state_known"] = true;
      details["mounted"] = state["mounted"];
      for (const auto *key : {"mountpoint", "mount_health", "mount_fs_type", "mount_source",
                             "source_available", "generation_verified", "source_changed"})
        if (state.contains(key)) details[key] = state[key];
      if (state.contains("mountpoint"))
        details["image"]["paths"]["mountpoint"] = state["mountpoint"];
    } else {
      details["mount_error"] = response.contains("error") && response["error"].is_object()
          ? response["error"] : J{{"reason", "invalid_snapshot"}, {"message", "Invalid mount state response"}};
    }
  } catch (std::exception const &error) {
    details["mount_error"] = {{"message", error.what()}};
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
