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

Q_DECLARE_METATYPE(ArchiveEntry)
Q_DECLARE_METATYPE(QVector<ArchiveEntry>)

// 所有耗时操作都在此对象中执行；对象会被移动到独立 QThread。
class ArchiveWorker : public QObject
{
    Q_OBJECT

public:
    enum Operation {
        List = 0,
        Extract = 1,
        Compress = 2
    };
    Q_ENUM(Operation)

    explicit ArchiveWorker(QObject *parent = nullptr);

    // 线程安全：供 UI 线程直接调用，后台循环会定期检查该标志。
    void requestCancel();

public slots:
    void listArchive(const QString &archivePath);
    void extractArchive(const QString &archivePath, const QString &destinationDir);
    void compressToZip(const QStringList &sources, const QString &zipPath);

signals:
    void progress(int percent, const QString &detail);
    void listed(const QVector<ArchiveEntry> &entries,
                const QString &formatName,
                qint64 totalBytes,
                int entryCount,
                bool truncated);
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
