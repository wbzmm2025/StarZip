#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

#include "ArchiveWorker.h"

class QCloseEvent;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTabWidget;
class QThread;
class QTreeWidget;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void chooseArchive();
    void chooseExtractTarget();
    void extractHere();
    void extractToChosen();
    void addFiles();
    void addFolder();
    void removeSelectedSources();
    void clearSources();
    void createZip();
    void cancelCurrentTask();

    void onWorkerProgress(int percent, const QString &detail);
    void onArchiveListed(const QVector<ArchiveEntry> &entries, const QString &formatName,
                         qint64 totalBytes, int entryCount, bool truncated);
    void onWorkerFinished(int operation, bool success, bool canceled, const QString &message,
                          int errorCode, const QString &outputPath);

private:
    void setupUi();
    void openArchive(const QString &path);
    void startExtraction(const QString &destination);
    bool beginJob(ArchiveWorker::Operation operation, const QString &busyText,
                  bool indeterminate, const std::function<void(ArchiveWorker *)> &task);
    void finishJobUi();
    void updateActions();
    void setDropZoneActive(bool active);
    void appendLog(const QString &text);
    void showError(const QString &title, const QString &message);
    void showSuccess(const QString &title, const QString &message);
    void addSourcePath(const QString &path);
    QStringList selectedSources() const;
    void refreshSourcesSummary();
    void fillArchiveTree(const QVector<ArchiveEntry> &entries);
    void updateArchiveHeader(const QString &formatName, qint64 totalBytes, int entryCount,
                             bool truncated);
    static QString humanSize(qint64 bytes);
    static QString operationLabel(int operation);
    static QString firstLine(const QString &text);

    QFrame *m_dropZone = nullptr;
    QLabel *m_dropTitle = nullptr;
    QLabel *m_dropHint = nullptr;
    QLabel *m_archiveLabel = nullptr;
    QLabel *m_archiveInfoLabel = nullptr;
    QLabel *m_targetLabel = nullptr;
    QLabel *m_progressLabel = nullptr;
    QLineEdit *m_targetEdit = nullptr;
    QPushButton *m_selectButton = nullptr;
    QPushButton *m_extractHereButton = nullptr;
    QPushButton *m_extractToButton = nullptr;
    QPushButton *m_browseTargetButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_addFilesButton = nullptr;
    QPushButton *m_addFolderButton = nullptr;
    QPushButton *m_removeSourceButton = nullptr;
    QPushButton *m_clearSourcesButton = nullptr;
    QPushButton *m_createZipButton = nullptr;
    QTabWidget *m_tabs = nullptr;
    QWidget *m_archivePage = nullptr;
    QWidget *m_sourcesPage = nullptr;
    QWidget *m_logPage = nullptr;
    QTreeWidget *m_archiveTree = nullptr;
    QTreeWidget *m_sourcesTree = nullptr;
    QPlainTextEdit *m_logView = nullptr;
    QProgressBar *m_progressBar = nullptr;

    QString m_archivePath;
    QString m_extractTarget;
    QString m_formatName;
    QVector<ArchiveEntry> m_entries;

    bool m_busy = false;
    bool m_closing = false;
    ArchiveWorker::Operation m_currentOperation = ArchiveWorker::List;
    QPointer<QThread> m_thread;
    QPointer<ArchiveWorker> m_worker;
};
