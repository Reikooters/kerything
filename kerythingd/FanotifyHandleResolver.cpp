#include "FanotifyHandleResolver.h"

#include <cerrno>
#include <cstring>
#include <memory>

#include <QByteArray>
#include <QFile>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
    ResolvedFanotifyHandle resolvedFromStat(const struct stat& st)
    {
        ResolvedFanotifyHandle resolved;
        resolved.ok = true;
        resolved.inode = static_cast<quint64>(st.st_ino);
        resolved.size = static_cast<quint64>(st.st_size);
        resolved.modificationTime = static_cast<qint64>(st.st_mtim.tv_sec);
        resolved.mode = static_cast<quint32>(st.st_mode);
        resolved.isDirectory = S_ISDIR(st.st_mode);
        return resolved;
    }

    QString errnoText(const char* operation)
    {
        return QStringLiteral("%1: %2")
            .arg(QString::fromLatin1(operation),
                 QString::fromLocal8Bit(std::strerror(errno)));
    }
}

FanotifyHandleResolver::FanotifyHandleResolver(QString mountPoint)
    : mountPoint_(std::move(mountPoint))
{
}

int FanotifyHandleResolver::openMountFd(QString* errorText) const
{
    const QByteArray mountPointNative = QFile::encodeName(mountPoint_);

    const int fd = ::open(
        mountPointNative.constData(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC
    );

    if (fd < 0 && errorText) {
        *errorText = QStringLiteral("failed to open mount point %1: %2")
            .arg(mountPoint_, QString::fromLocal8Bit(std::strerror(errno)));
    }

    return fd;
}

int FanotifyHandleResolver::openHandle(
    const QString& handleHex,
    qint32 handleType,
    QString* errorText) const
{
    const QByteArray handleBytes = QByteArray::fromHex(handleHex.toLatin1());

    if (handleBytes.isEmpty()) {
        if (errorText) {
            *errorText = QStringLiteral("empty fanotify file handle");
        }

        return -1;
    }

    const qsizetype allocationSize =
        static_cast<qsizetype>(sizeof(struct file_handle)) + handleBytes.size();

    auto storage = std::make_unique<unsigned char[]>(
        static_cast<std::size_t>(allocationSize)
    );

    auto* handle = reinterpret_cast<struct file_handle*>(storage.get());
    handle->handle_bytes = static_cast<unsigned int>(handleBytes.size());
    handle->handle_type = static_cast<int>(handleType);

    std::memcpy(
        handle->f_handle,
        handleBytes.constData(),
        static_cast<std::size_t>(handleBytes.size())
    );

    QString mountError;
    const int mountFd = openMountFd(&mountError);
    if (mountFd < 0) {
        if (errorText) {
            *errorText = mountError;
        }

        return -1;
    }

    const int fd = ::open_by_handle_at(
        mountFd,
        handle,
        O_RDONLY | O_CLOEXEC
    );

    const int savedErrno = errno;
    ::close(mountFd);

    if (fd < 0 && errorText) {
        errno = savedErrno;
        *errorText = QStringLiteral("open_by_handle_at failed for %1: %2")
            .arg(mountPoint_, QString::fromLocal8Bit(std::strerror(errno)));
    }

    return fd;
}

ResolvedFanotifyHandle FanotifyHandleResolver::resolveObjectHandle(
    const QString& handleHex,
    qint32 handleType) const
{
    QString errorText;
    const int fd = openHandle(handleHex, handleType, &errorText);

    if (fd < 0) {
        ResolvedFanotifyHandle resolved;
        resolved.errorText = errorText;
        return resolved;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ResolvedFanotifyHandle resolved;
        resolved.errorText = errnoText("fstat failed");
        ::close(fd);
        return resolved;
    }

    ::close(fd);
    return resolvedFromStat(st);
}

ResolvedFanotifyHandle FanotifyHandleResolver::resolveChildByParentHandleAndName(
    const QString& parentHandleHex,
    qint32 parentHandleType,
    const QString& name) const
{
    if (name.isEmpty()) {
        ResolvedFanotifyHandle resolved;
        resolved.errorText = QStringLiteral("empty child name");
        return resolved;
    }

    QString errorText;
    const int parentFd = openHandle(parentHandleHex, parentHandleType, &errorText);

    if (parentFd < 0) {
        ResolvedFanotifyHandle resolved;
        resolved.errorText = errorText;
        return resolved;
    }

    const QByteArray nameNative = QFile::encodeName(name);

    struct stat st {};
    if (::fstatat(
            parentFd,
            nameNative.constData(),
            &st,
            AT_SYMLINK_NOFOLLOW
        ) != 0) {
        ResolvedFanotifyHandle resolved;
        resolved.errorText = errnoText("fstatat failed");
        ::close(parentFd);
        return resolved;
    }

    ::close(parentFd);
    return resolvedFromStat(st);
}