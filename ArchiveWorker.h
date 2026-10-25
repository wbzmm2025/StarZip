#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>

// 压缩包中的一个条目（仅保存 UI 展示所需的最小元数据）。
struct ArchiveEntry
{
    QString path;
    qint64 size = 0;
    QDateTime modified;
    bool isDir = false;
    bool encrypted = false;
};

// 压缩参数。format 取值：zip / 7z / tar / tar.gz / tar.xz。
struct CompressOptions
{
    QString format = QStringLiteral("zip");
    int level = 6;
    QString password;
};

Q_DECLARE_METATYPE(ArchiveEntry)
Q_DECLARE_METATYPE(QVector<ArchiveEntry>)
Q_DECLARE_METATYPE(CompressOptions)

// 所有耗时操作都在此对象中执行；对象会被移动到独立 QThread。
class ArchiveWorker : public QObject
{
    Q_OBJECT

public:
    enum Operation {
        List = 0,
        Extract = 1,
        Compress = 2,
        Test = 3
    };
    Q_ENUM(Operation)

    explicit ArchiveWorker(QObject *parent = nullptr);

    // 线程安全：供 UI 线程直接调用，后台循环会定期检查该标志。
    void requestCancel();

public slots:
    void listArchive(const QString &archivePath, const QString &password = QString());
    void extractArchive(const QString &archivePath, const QString &destinationDir,
                        const QString &password = QString(),
                        const QStringList &onlyPaths = QStringList());
    void testArchive(const QString &archivePath, const QString &password = QString());
    void compressArchive(const QStringList &sources, const QString &outputPath,
                         const CompressOptions &options);

signals:
    void progress(int percent, const QString &detail);
    void listed(const QVector<ArchiveEntry> &entries,
                const QString &formatName,
                qint64 totalBytes,
                int entryCount,
                bool truncated);
    // 压缩包需要密码，或用户提供的密码不正确。UI 可提示后重试。
    void passwordRequired(int operation, const QString &archivePath);
    void finished(int operation,
                  bool success,
                  bool canceled,
                  const QString &message,
                  int errorCode,
                  const QString &outputPath);

private:
    bool isCancelRequested() const;
    void resetCancel();

    std::atomic_bool m_cancelRequested{false};
};
