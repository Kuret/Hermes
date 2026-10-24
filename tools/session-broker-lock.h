// SPDX-License-Identifier: GPL-3.0-only
/** @file session-broker-lock.h
 *  @brief Fail-closed serialization of privileged account and device mutations.
 */
#pragma once

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hermes::session_broker {
  /**
   * @brief Own an exclusive, nonblocking lock on a trusted regular file.
   *
   * Symlinks, hard links, foreign ownership and writable group/other access
   * are refused. Contention fails immediately so a second broker cannot hang
   * its serial request loop behind a different process.
   */
  class mutation_lock {
  public:
    /** @param path Broker-owned lock file under its private runtime directory. */
    explicit mutation_lock(const char *path) {
      const int candidate = ::open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
      if (candidate < 0) {
        return;
      }
      struct stat info {};
      if (::fstat(candidate, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || info.st_nlink != 1 || (info.st_mode & (S_IWGRP | S_IWOTH)) != 0 || ::flock(candidate, LOCK_EX | LOCK_NB) != 0) {
        ::close(candidate);
        return;
      }
      fd_ = candidate;
    }

    mutation_lock(const mutation_lock &) = delete;
    mutation_lock &operator=(const mutation_lock &) = delete;

    ~mutation_lock() {
      if (fd_ >= 0) {
        ::close(fd_);
      }
    }

    /** @return True only while this object owns the exclusive lock. */
    explicit operator bool() const {
      return fd_ >= 0;
    }

  private:
    int fd_ = -1;
  };
}  // namespace hermes::session_broker
