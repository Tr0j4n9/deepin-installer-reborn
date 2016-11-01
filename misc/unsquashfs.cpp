// Copyright (c) 2016 Deepin Ltd. All rights reserved.
// Use of this source is governed by General Public License that can be found
// in the LICENSE file.

#define _XOPEN_SOURCE 500  // Required by nftw().
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "base/command.h"
#include "base/file_util.h"

#define S_IMODE 0777

namespace {

const char kAppName[] = "deepin-installer-unsqaushfs";
const char kAppDesc[] = "Tool to extract squashfs filesystem";
const char kAppVersion[] = "0.0.1";

// Default folder name of target.
const char kDefaultDest[] = "squashfs-root";

// Absolute folder path to mount filesystem to.
const char kMountPoint[] = "/dev/shm/installer-unsquashfs";

const int kExitOk = 0;
const int kExitErr = 1;

// Maximum number of opended file descriptors.
// See /proc/self/limits for more information.
const int kMaxOpenFd = 256;

// Global references to progress_file, src_dir and dest_dir.
QString g_progress_file;
QString g_src_dir;
QString g_dest_dir;

// Copy regular file with sendfile() system call, from |src_file| to
// |dest_file|. Size of |src_file| is |file_size|.
bool SendFile(const char* src_file, const char* dest_file, ssize_t file_size) {
  qDebug() << "SendFile():" << src_file << dest_file;
  int src_fd, dest_fd;
  src_fd = open(src_file, O_RDONLY);
  if (src_fd == -1) {
    qCritical() << "SendFile() Failed to open src file:" << src_file;
    perror("Open src file failed!");
    return 1;
  }

  // TODO(xushaohua): handles umask
  dest_fd = open(dest_file, O_CREAT | O_RDWR, S_IREAD | S_IWRITE);
  if (dest_fd == -1) {
    qCritical() << "SendFile() Failed to open dest file:" << dest_file;
    perror("Open dest file failed!");
    return 1;
  }

  size_t num_to_read = size_t(file_size);
  bool ok = true;
  while (num_to_read > 0) {
    const ssize_t num_sent = sendfile(dest_fd, src_fd, NULL, num_to_read);
    if (num_sent <= 0) {
      perror("sendfile() err:");
      ok = false;
      break;
    }
    num_to_read -= num_sent;
  }

  close(src_fd);
  close(dest_fd);

  return ok;
}

bool CopySymLink(const char* src_file, const char* dest_file) {
  qDebug() << "CopySymLink():" << src_file << dest_file;

  char buf[PATH_MAX];
  ssize_t link_len = readlink(src_file, buf, PATH_MAX);
  if (link_len <= 0) {
    qCritical() << "CopySymLink() readlink() failed:" << src_file;
    perror("readlink()");
    return false;
  }

  char linkpath[link_len + 1];
  strncpy(linkpath, buf, size_t(link_len));
  linkpath[link_len] = '\0';
  qDebug() << "link path:" << linkpath;
  if (symlink(linkpath, dest_file) != 0) {
    qCritical() << "CopySymLink() symlink() failed:" << linkpath << dest_file;
    perror("symlink()");
    return false;
  } else {
    return true;
  }
}

// Update xattr (access control lists and file capabilities)
bool CopyXAttr(const char* src_file, const char* dest_file) {
  qDebug() << "CopyXAttr():" << src_file << dest_file;
  bool ok = true;
  // size of extended attribute list, 64k.
  char list[XATTR_LIST_MAX];
  char value[XATTR_NAME_MAX];
  ssize_t xlist_len = llistxattr(src_file, list, XATTR_LIST_MAX);
  if (xlist_len < 0) {
    qCritical() << "CopyXAttr() listxattr() failed:" << src_file;
    perror("listxattr()");
    ok = false;
  } else {
    ssize_t value_len;
    for (int ns = 0; ns < xlist_len; ns += strlen(&list[ns] + 1)) {
      value_len = lgetxattr(src_file, &list[ns], value, XATTR_NAME_MAX);
      if (value_len == -1) {
        // FIXME(xushaohua):
        qWarning() << "CopyXAttr() could not get value:" << src_file;
        qDebug() << ns << xlist_len;
        break;
      } else {
        if (lsetxattr(dest_file, &list[ns], value, size_t(value_len), 0) != 0) {
          qCritical() << "CopyXAttr() setxattr() failed:" << dest_file
                      << &list[ns] << value;
          perror("setxattr()");
          ok = false;
          break;
        }
      }
    }
  }

  return ok;
}

// Tree walk handler. Copy one item from |fpath|.
int CopyItem(const char* fpath, const struct stat* sb,
             int typeflag, struct FTW* ftwbuf) {
  Q_UNUSED(sb);
  Q_UNUSED(typeflag);
  Q_UNUSED(ftwbuf);

  qDebug() << "CopyItem()" << fpath;

  struct stat st;
  if (lstat(fpath, &st) != 0) {
    qCritical() << "CopyItem() call lstat() failed:" << fpath;
    perror("lstat()");
    return 1;
  }

  QString relative_path = QString(fpath).remove(g_src_dir);
  if (relative_path.startsWith('/')) {
    relative_path = relative_path.mid(1);
  }
  const QString dest_filepath =
      QDir(g_dest_dir).absoluteFilePath(relative_path);

  // Create paretn dirs.
  installer::CreateParentDires(dest_filepath);

  const size_t dest_len = size_t(dest_filepath.length());
  char dest_file[dest_len + 1];
  strncpy(dest_file, dest_filepath.toUtf8().data(), dest_len);
  dest_file[dest_len] = '\0';

  bool ok = true;

  // Get file mode.
  const mode_t mode = st.st_mode & S_IMODE;

  if (S_ISLNK(st.st_mode)) {
    // Symbolic link
    ok = CopySymLink(fpath, dest_file);
  } else if (S_ISREG(sb->st_mode)) {
    // Regular file
    ok = SendFile(fpath, dest_file, st.st_size);
  } else if (S_ISDIR(st.st_mode)) {
    // Directory
    ok = installer::CreateDirs(dest_filepath);
  } else if (S_ISCHR(st.st_mode)) {
    // Character device
    ok = (mknod(dest_file, mode | S_IFCHR, st.st_dev) == 0);
  } else if (S_ISBLK(st.st_mode)) {
    // For block device.
    ok = (mknod(dest_file, mode | S_IFBLK, st.st_dev) == 0);
  } else if (S_ISFIFO(st.st_mode)) {
    // FIFO
    ok = (mknod(dest_file, mode | S_IFIFO, st.st_dev) == 0);
  } else if (S_ISSOCK(st.st_mode)) {
    // Socket
    ok = (mknod(dest_file, mode | S_IFSOCK, st.st_dev) == 0);
  } else {
    qCritical() << "CopyItem() Unknown file mode:" << st.st_mode;
  }

  // TODO(xushaohua): Keep file descriptor.
  if (!S_ISLNK(st.st_mode)) {
    if (chmod(dest_file, mode) != 0) {
      qCritical() << "CopyItem() chmod failed:" << dest_file << mode;
      perror("chmod()");
      ok = false;
    }
  }
  // Update permissions
  if (lchown(dest_file, st.st_uid, st.st_gid) != 0) {
    qCritical() << "CopyItem() lchown() failed:" << dest_file
                << st.st_uid << st.st_gid;
    perror("lchown()");
    ok = false;
  }

  if (!CopyXAttr(fpath, dest_file)) {
    ok = false;
  }

  // TODO(xushaohua): Update progress bar

  return ok ? 0 : 1;
}

// Copy files from |mount_point| to |dest_dir|, keeping xattrs.
bool CopyFiles(const QString& src_dir, const QString& dest_dir,
               const QString& progress_file) {
  qDebug() << "CopyFiles():" << src_dir << dest_dir << progress_file;
  if (!installer::CreateDirs(dest_dir)) {
    qCritical() << "CopyFiles() create dest dir failed:" << dest_dir;
    return false;
  }

  const mode_t old_mask = umask(0);

  g_progress_file = progress_file;
  g_src_dir = src_dir;
  g_dest_dir = dest_dir;

  const bool ok = (nftw(src_dir.toUtf8().data(), CopyItem,
                        kMaxOpenFd, FTW_PHYS) == 0);

  // Reset umask.
  umask(old_mask);

  return ok;
}

// Mount filesystem at |src| to |mount_point|
bool MountFs(const QString& src, const QString& mount_point) {
  if (!installer::CreateDirs(mount_point)) {
    qCritical() << "MountFs() Failed to create folder:" << mount_point;
    return false;
  }
  QString output, err;
  const bool ok = installer::SpawnCmd("mount", {src, mount_point}, output, err);
  if (!ok) {
    qCritical() << "MountFs() err:" << err;
  }

  // TODO(xushaohua): Check |mount_point| dir is not empty.

  return ok;
}

// Umount filesystem from |mount_point|.
bool UnMountFs(const QString& mount_point) {
  QString out, err;
  const bool ok = installer::SpawnCmd("umount", {mount_point}, out, err);
  if (!ok) {
    qCritical() << "Umount err:" << err;
  }
  return ok;
}

}  // namespace

int main(int argc, char* argv[]) {
  // Parse args
  // Mount squashfs file
  // Copy files to target
  // Unmount from mount-point

  QCoreApplication app(argc, argv);
  app.setApplicationVersion(kAppVersion);

  QCommandLineParser parser;
  const QCommandLineOption dest_option(
      "dest", "extract to <pathname>, default \"squashfs-root\"",
      "pathname", kDefaultDest);
  parser.addOption(dest_option);
  const QCommandLineOption progress_option(
      "progress","print progress info to \"file\"",
      "file", "");
  parser.addOption(progress_option);
  parser.setApplicationDescription(kAppDesc);
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument("file", "squashfs filesystem to be extracted");

  if (!parser.parse(app.arguments())) {
    parser.showHelp(kExitErr);
  }

  if (parser.isSet("version") || parser.isSet("help")) {
    // Show help and exit.
    parser.showHelp(kExitOk);
  }

  const QStringList positional_args = parser.positionalArguments();
  if (positional_args.length() != 1) {
    qCritical() << "Too many files to extract, expect one!";
    parser.showHelp(kExitErr);
  }

  const QString src(positional_args.at(0));
  const QFile src_file(src);
  if (!src_file.exists()) {
    qCritical() << "File not found!:" << src;
    parser.showHelp(kExitErr);
  }
  if (src_file.size() == 0) {
    qCritical() << "Filesystem is empty!" << src;
    parser.showHelp(kExitErr);
  }

  const QString mount_point(kMountPoint);

  const QString dest_dir = parser.value(dest_option);
  const QString progress_file = parser.value(progress_option);

  if (!MountFs(src, mount_point)) {
    qCritical() << "Mount" << src << "to" << mount_point << "failed!";
    exit(kExitErr);
  }

  const bool ok = CopyFiles(mount_point, dest_dir, progress_file);
  if (!ok) {
    qCritical() << "Copy files failed!";
  }

  if (!UnMountFs(mount_point)) {
    qCritical() << "UnMount failed:" << mount_point;
    exit(kExitErr);
  }

  exit(ok ? kExitOk : kExitErr);
}