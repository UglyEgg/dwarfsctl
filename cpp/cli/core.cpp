// SPDX-License-Identifier: GPL-3.0-or-later
#include "core.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/file.h>
#include <unistd.h>
namespace ctl {
std::string Input::get(std::string const &k, std::string fallback) const {
  auto it = options.find(k);
  return it == options.end() ? fallback : it->second.back();
}
std::vector<std::string> Input::many(std::string const &k) const {
  auto it = options.find(k);
  return it == options.end() ? std::vector<std::string>{} : it->second;
}
std::string hash(std::string const &s) {
  return QCryptographicHash::hash(QByteArray::fromStdString(s),
                                  QCryptographicHash::Sha256)
      .toHex()
      .toStdString();
}
std::string resolved_path(fs::path const &p) {
  return fs::weakly_canonical(fs::absolute(p)).string();
}
fs::path output_path(fs::path const &p) {
  // Resolve the containing directory so equivalent relative spellings share a
  // lock and staging directory, but never dereference the final component.
  // Publication must replace an output symlink itself rather than overwrite
  // the file it names.
  auto absolute = fs::absolute(p).lexically_normal();
  return fs::weakly_canonical(absolute.parent_path()) / absolute.filename();
}
bool path_exists(fs::path const &p) {
  std::error_code ec;
  auto s = fs::symlink_status(p, ec);
  return !ec && s.type() != fs::file_type::not_found;
}
std::string engine_path() {
  if (auto v = std::getenv("DWARFSCTL_ENGINE"))
    return fs::absolute(v).string();
  return (fs::path(str(QCoreApplication::applicationDirPath())).parent_path() /
          "libexec/dwarfsctl-engine")
      .string();
}
fs::path runtime_mount_root() {
  auto run = str(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation));
  if (run.empty())
    run = "/run/user/" + std::to_string(getuid());
  return fs::path(run) / "dwarfs";
}
Run execute(std::string const &program, std::vector<std::string> const &args,
            std::optional<fs::path> output, std::optional<fs::path> cwd) {
  QProcess proc;
  proc.setProgram(qs(program));
  QStringList argv;
  for (auto const &a : args)
    argv.append(qs(a));
  proc.setArguments(argv);
  proc.setStandardInputFile(QProcess::nullDevice());
  if (cwd)
    proc.setWorkingDirectory(qs(cwd->string()));
  if (output)
    proc.setStandardOutputFile(qs(output->string()), QIODevice::Truncate);
  proc.start();
  if (!proc.waitForStarted())
    throw Error(3,
                "could not start " + program + ": " + str(proc.errorString()));
  // QProcess drains both channels while waiting, avoiding pipe deadlocks. File
  // output bypasses memory capture for large cat/checksum operations.
  QByteArray capturedError, pending;
  std::string phase;
  const bool building = !args.empty() && args[0] == "mkdwarfs" &&
                        std::find(args.begin(), args.end(), "-i") != args.end();
  do {
    proc.waitForFinished(100);
    auto chunk = proc.readAllStandardError();
    capturedError += chunk;
    if (building) {
      pending += chunk;
      int newline;
      while ((newline = pending.indexOf('\n')) >= 0) {
        auto line = pending.left(newline);
        pending.remove(0, newline + 1);
        std::string next;
        if (line.contains("scanning "))
          next = "scanning";
        else if (line.contains("building blocks") ||
                 line.contains("compression to finish") ||
                 line.contains("segmenting/blockifying"))
          next = "compressing";
        else if (line.contains("saving ") || line.contains("building metadata"))
          next = "metadata";
        if (!next.empty() && next != phase) {
          phase = next;
          std::cerr << J({{"event", "build_progress"}, {"phase", phase}}).dump()
                    << std::endl;
        }
      }
    }
  } while (proc.state() != QProcess::NotRunning);
  Run result{proc.exitStatus() == QProcess::NormalExit ? proc.exitCode() : 9,
             proc.readAllStandardOutput(), capturedError, J::object()};
  std::vector<std::string> full{program};
  full.insert(full.end(), args.begin(), args.end());
  result.payload = {{"tool", args.empty() ? program : args[0]},
                    {"argv", full},
                    {"exit_code", result.code}};
  if (!result.out.isEmpty())
    result.payload["stdout"] = str(QString::fromUtf8(result.out.left(8192)));
  if (!result.err.isEmpty())
    result.payload["stderr"] = str(QString::fromUtf8(result.err.left(8192)));
  if (result.out.size() > 8192)
    result.payload["stdout_truncated"] = true;
  if (result.err.size() > 8192)
    result.payload["stderr_truncated"] = true;
  if (proc.exitStatus() != QProcess::NormalExit)
    result.payload["termination"] = "engine process crashed";
  return result;
}
Run engine(std::string const &op, std::vector<std::string> const &args,
           std::optional<fs::path> output) {
  auto full = args;
  full.insert(full.begin(), op);
  return execute(engine_path(), full, output);
}
void require_success(Run const &r) {
  if (r.code)
    throw Error(r.code == 4 || r.err.contains("no filesystem found") ? 4 : 9,
                "DwarFS operation failed (" + std::to_string(r.code) +
                    "): " + str(QString::fromUtf8(r.err.left(8192))),
                {{"tool", r.payload}});
}
Temp::Temp(fs::path const &dir) {
  fs::create_directories(dir);
  auto name = (dir / ".dwarfsctl-XXXXXX").string();
  std::vector<char> buffer(name.begin(), name.end());
  buffer.push_back(0);
  int fd = ::mkstemp(buffer.data());
  if (fd < 0)
    throw Error(9, "cannot create staging file: " +
                       std::string(std::strerror(errno)));
  ::close(fd);
  path = buffer.data();
}
Temp::~Temp() {
  std::error_code ec;
  fs::remove(path, ec);
}
void publish(fs::path const &staged, fs::path const &dest, bool overwrite) {
  // Both files are siblings. link() publishes atomically without clobbering;
  // rename() atomically replaces the directory entry, including symlinks.
  int rc = overwrite ? ::rename(staged.c_str(), dest.c_str())
                     : ::link(staged.c_str(), dest.c_str());
  if (rc != 0)
    throw Error(errno == EEXIST ? 4 : 9, "cannot publish " + dest.string() +
                                             ": " + std::strerror(errno));
}
void atomic_write(fs::path const &path, std::string const &data) {
  fs::create_directories(path.parent_path());
  QSaveFile file(qs(path.string()));
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(data.data(), data.size()) !=
          static_cast<qint64>(data.size()) ||
      !file.commit())
    throw Error(9, "cannot save " + path.string() + ": " +
                       str(file.errorString()));
}
Lock::Lock(fs::path const &path) {
  fs::create_directories(path.parent_path());
  fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    throw Error(9, "cannot open lock " + path.string());
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    fd = -1;
    throw Error(5, "operation lock busy: " + path.string());
  }
}
Lock::~Lock() {
  if (fd >= 0)
    ::close(fd);
}
J envelope(std::string const &cmd, J const &image, J const &details,
           J const &warnings, Error const *error) {
  J strings = J::array();
  for (auto const &w : warnings)
    strings.push_back(w.at("message"));
  J e = nullptr;
  if (error) {
    static const std::map<int, std::string> kinds{
        {2, "BadCliUsage"}, {3, "DependencyMissing"}, {4, "InvalidPath"},
        {5, "LockBusy"},    {6, "MountStateError"},   {9, "Internal"}};
    e = {{"code", error->code},
         {"kind",
          kinds.contains(error->code) ? kinds.at(error->code) : "Internal"},
         {"message", error->what()}};
  }
  return {{"ok", !error},        {"command", cmd},
          {"image", image},      {"details", details},
          {"warnings", strings}, {"warning_objects", warnings},
          {"error", e}};
}
} // namespace ctl
