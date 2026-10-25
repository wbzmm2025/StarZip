#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

#include "ArchiveWorker.h"

class QAction;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QPoint;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QThread;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // 打开启动参数/外部调用指定的压缩包（例如 StarZip.exe archive.zip）。
    void openArchivePath(const QString &path);

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
    void extractSelected();
    void testArchive();
    void addFiles();
    void addFolder();
    void removeSelectedSources();
    void clearSources();
    void browseOutput();
    void createArchive();
    void startNewArchive();
    void cancelCurrentTask();

    void togglePasswordVisible(bool visible);
    void onFormatChanged();
    void onArchiveSelectionChanged();
    void filterArchiveEntries(const QString &text);
    void showArchiveContextMenu(const QPoint &pos);
    void showLogTab();
    void showAbout();

    void onWorkerProgress(int percent, const QString &detail);
    void onPasswordRequired(int operation, const QString &archivePath);
    void onArchiveListed(const QVector<ArchiveEntry> &entries, const QString &formatName,
                         qint64 totalBytes, int entryCount, bool truncated);
    void onWorkerFinished(int operation, bool success, bool canceled, const QString &message,
                          int errorCode, const QString &outputPath);

private:
    struct FormatChoice {
        QString id;
        QString label;
        QString suffix;
        QString dialogFilter;
        bool supportsPassword = false;
        bool supportsLevel = true;
    };

    void setupUi();
    void setupActions();
    void setupMenus();
    void setupToolBar();
    void applyStyle();
    QWidget *createEmptyPage();
    QWidget *createArchivePage();
    QWidget *createCompressPage();
    QWidget *createLogPage();
    void rebuildRecentMenu();
    void addRecentFile(const QString &path);
    void clearRecentFiles();

    void openArchive(const QString &path, const QString &password = QString(),
                     bool retry = false);
    void startExtraction(const QString &destination, const QStringList &onlyPaths,
                         const QString &password, bool retry = false);
    void startTest(const QString &password, bool retry = false);
    bool beginJob(ArchiveWorker::Operation operation, const QString &busyText,
                  bool indeterminate, const std::function<void(ArchiveWorker *)> &task);
    void cleanupThread();
    void resetArchiveState();
    void updateActions();
    void updateStatusInfo();
    void updateArchiveHeader(const QString &formatName, qint64 totalBytes, int entryCount,
                             bool truncated);
    void appendLog(const QString &text);
    void showError(const QString &title, const QString &message);
    void showSuccess(const QString &title, const QString &message,
                     const QString &folderPath = QString());
    QString promptPassword(const QString &title, const QString &text);
    QStringList selectedArchivePaths() const;
    bool archiveHasEncryptedEntries() const;

    void addSourcePath(const QString &path);
    QStringList selectedSources() const;
    void refreshSourcesSummary();
    void updateSuggestedOutput();

    void fillArchiveTree(const QVector<ArchiveEntry> &entries);
    bool filterTreeItem(QTreeWidgetItem *item, const QString &needle);
    QString currentFormat() const;
    FormatChoice currentFormatChoice() const;
    static QVector<FormatChoice> formatChoices();
    static QString ensureSuffix(const QString &path, const QString &suffix);
    static QString replaceSuffix(const QString &path, const QString &suffix);
    static QString humanSize(qint64 bytes);
    static QString operationLabel(int operation);
    static QString firstLine(const QString &text);
    static QString archiveDisplayName(const QString &path);

    QTabWidget *m_tabs = nullptr;
    QWidget *m_archivePage = nullptr;
    QWidget *m_compressPage = nullptr;
    QWidget *m_logPage = nullptr;
    QStackedWidget *m_archiveStack = nullptr;
    QWidget *m_emptyPage = nullptr;
    QWidget *m_contentPage = nullptr;

    QLabel *m_emptyTitle = nullptr;
    QLabel *m_emptyHint = nullptr;
    QLabel *m_emptyFormats = nullptr;
    QPushButton *m_emptyOpenButton = nullptr;
    QPushButton *m_emptyNewButton = nullptr;
    QLabel *m_emptyIcon = nullptr;

    QLabel *m_archiveLabel = nullptr;
    QLabel *m_archiveInfoLabel = nullptr;
    QLabel *m_formatBadge = nullptr;
    QLabel *m_targetLabel = nullptr;
    QLineEdit *m_targetEdit = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QPushButton *m_openButton = nullptr;
    QPushButton *m_browseTargetButton = nullptr;
    QPushButton *m_extractHereButton = nullptr;
    QPushButton *m_extractToButton = nullptr;
    QPushButton *m_extractSelectedButton = nullptr;
    QPushButton *m_testButton = nullptr;
    QTreeWidget *m_archiveTree = nullptr;

    QTreeWidget *m_sourcesTree = nullptr;
    QPushButton *m_addFilesButton = nullptr;
    QPushButton *m_addFolderButton = nullptr;
    QPushButton *m_removeSourceButton = nullptr;
    QPushButton *m_clearSourcesButton = nullptr;
    QComboBox *m_formatCombo = nullptr;
    QComboBox *m_levelCombo = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QCheckBox *m_showPasswordCheck = nullptr;
    QLabel *m_passwordNote = nullptr;
    QLineEdit *m_outputEdit = nullptr;
    QPushButton *m_browseOutputButton = nullptr;
    QPushButton *m_createArchiveButton = nullptr;

    QPlainTextEdit *m_logView = nullptr;
    QLabel *m_progressLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QLabel *m_statusFormatLabel = nullptr;
    QLabel *m_statusCountLabel = nullptr;

    QAction *m_openAction = nullptr;
    QAction *m_newAction = nullptr;
    QAction *m_addFilesAction = nullptr;
    QAction *m_addFolderAction = nullptr;
    QAction *m_compressAction = nullptr;
    QAction *m_extractHereAction = nullptr;
    QAction *m_extractToAction = nullptr;
    QAction *m_extractSelectedAction = nullptr;
    QAction *m_testAction = nullptr;
    QAction *m_cancelAction = nullptr;
    QAction *m_logAction = nullptr;
    QAction *m_aboutAction = nullptr;
    QAction *m_exitAction = nullptr;
    QToolBar *m_toolBar = nullptr;
    QMenu *m_recentMenu = nullptr;

    QString m_archivePath;
    QString m_archivePassword;
    QString m_extractTarget;
    QString m_formatName;
    QString m_lastExtractDest;
    QStringList m_lastExtractOnly;
    QVector<ArchiveEntry> m_entries;

    bool m_busy = false;
    bool m_uiReady = false;
    bool m_closing = false;
    bool m_passwordSignal = false;
    int m_passwordAttempts = 0;
    ArchiveWorker::Operation m_currentOperation = ArchiveWorker::List;
    QPointer<QThread> m_thread;
    QPointer<ArchiveWorker> m_worker;
};
