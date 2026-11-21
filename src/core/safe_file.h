// Replacing a file the player cannot afford to lose (a battery save, a save
// state, a settings file): the bytes go to "<path>.tmp", reach the disk
// (fsync) and only then take the file's name, so a power cut or a full card
// leaves the previous file whole. Every step is checked.
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ds {

// `keep_as`: when given and `path` exists, the file being replaced is kept
// under that name (replacing an older one there) instead of being dropped.
inline bool write_file_atomic(const std::string& path, const void* data, size_t n, std::string& err,
                              const std::string& keep_as = std::string()) {
  const std::string tmp = path + ".tmp";
  const auto fail = [&](const char* what, const std::string& on) {
    err = std::string(what) + " " + on + ": " + std::strerror(errno);
    std::remove(tmp.c_str());
    return false;
  };
#if defined(_WIN32)
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return fail("cannot create", tmp);
  const bool wrote = std::fwrite(data, 1, n, f) == n && std::fflush(f) == 0;
  if (std::fclose(f) != 0 || !wrote) return fail("cannot write", tmp);
  if (!keep_as.empty()) { std::remove(keep_as.c_str()); std::rename(path.c_str(), keep_as.c_str()); }
  std::remove(path.c_str());   // rename does not replace here
  if (std::rename(tmp.c_str(), path.c_str()) != 0) return fail("cannot replace", path);
#else
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return fail("cannot create", tmp);
  const char* p = static_cast<const char*>(data);
  for (size_t left = n; left;) {
    const ssize_t w = ::write(fd, p, left);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) { const int e = errno; ::close(fd); errno = e; return fail("cannot write", tmp); }
    p += w; left -= static_cast<size_t>(w);
  }
  if (::fsync(fd) != 0 && errno != EINVAL && errno != EROFS) { const int e = errno; ::close(fd); errno = e; return fail("cannot sync", tmp); }
  if (::close(fd) != 0) return fail("cannot write", tmp);
  // The old file under its other name first: between the two renames the
  // save exists as keep_as, never as nothing.
  if (!keep_as.empty() && std::rename(path.c_str(), keep_as.c_str()) != 0 && errno != ENOENT) return fail("cannot keep the old", path);
  if (std::rename(tmp.c_str(), path.c_str()) != 0) return fail("cannot replace", path);
  // The rename itself is in the directory: without this a power cut can undo it.
  const size_t slash = path.find_last_of('/');
  const std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
  if (const int d = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); d >= 0) { ::fsync(d); ::close(d); }
#endif
  return true;
}

} // namespace ds
