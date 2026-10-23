#include "MainWindow.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QCloseEvent>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QThread>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {

bool urlIsLocalFile(const QUrl &url)
{
    return url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile();
}

QString archiveDisplayName(const QString &path)
{
    const QFileInfo info(path);
    return info.fileName().isEmpty() ? path : info.fileName();
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    qRegisterMetaType<ArchiveEntry>("ArchiveEntry");
    qRegisterMetaType<QVector<ArchiveEntry>>("QVector<ArchiveEntry>");

    setupUi();

    m_extractTarget = QDir::currentPath();
    m_targetEdit->setText(QDir::toNativeSeparators(m_extractTarget));
    m_targetEdit->setToolTip(QStringLiteral("默认解压到 StarZip 启动时的当前目录：%1")
                                 .arg(QDir::toNativeSeparators(m_extractTarget)));

    appendLog(QStringLiteral("StarZip 0.0.1 已启动。"));
    appendLog(QStringLiteral("支持通过 libarchive 识别/解压 ZIP、7Z、RAR 等格式；压缩输出为 ZIP。"));
    statusBar()->showMessage(QStringLiteral("就绪 — 拖入 ZIP / 7Z / RAR 压缩包，或点击“选择文件…”"));
    updateActions();
}

MainWindow::~MainWindow()
{
    if (m_worker)
        m_worker->requestCancel();
    if (m_thread && m_thread->isRunning()) {
        m_thread->quit();
        m_thread->wait(15000);
    }
}

void MainWindow::setupUi()
{
    setWindowTitle(QStringLiteral("StarZip 0.0.1"));
    resize(980, 720);
    setMinimumSize(820, 600);
    setAcceptDrops(true);

    auto *central = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(16, 14, 16, 12);
    rootLayout->setSpacing(10);

    // 顶部拖拽区
    m_dropZone = new QFrame(central);
    m_dropZone->setObjectName(QStringLiteral("dropZone"));
    m_dropZone->setProperty("dragActive", false);
    m_dropZone->setMinimumHeight(126);
    auto *dropLayout = new QVBoxLayout(m_dropZone);
    dropLayout->setContentsMargins(22, 18, 22, 18);
    dropLayout->setSpacing(6);
    m_dropTitle = new QLabel(QStringLiteral("把 ZIP / 7Z / RAR 压缩包拖到这里"), m_dropZone);
    m_dropTitle->setObjectName(QStringLiteral("dropTitle"));
    m_dropTitle->setAlignment(Qt::AlignCenter);
    m_dropHint = new QLabel(
        QStringLiteral("自动识别格式 · 仅接受本地文件 · 解压与压缩均在后台线程执行"), m_dropZone);
    m_dropHint->setObjectName(QStringLiteral("dropHint"));
    m_dropHint->setAlignment(Qt::AlignCenter);
    dropLayout->addStretch(1);
    dropLayout->addWidget(m_dropTitle);
    dropLayout->addWidget(m_dropHint);
    dropLayout->addStretch(1);
    rootLayout->addWidget(m_dropZone);

    // 当前压缩包信息
    auto *archiveRow = new QHBoxLayout();
    archiveRow->setSpacing(8);
    auto *archiveCaption = new QLabel(QStringLiteral("当前压缩包："), central);
    m_archiveLabel = new QLabel(QStringLiteral("未选择"), central);
    m_archiveLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_archiveLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    archiveRow->addWidget(archiveCaption);
    archiveRow->addWidget(m_archiveLabel, 1);
    rootLayout->addLayout(archiveRow);

    // 解压目标目录
    auto *targetRow = new QHBoxLayout();
    targetRow->setSpacing(8);
    m_targetLabel = new QLabel(QStringLiteral("解压目标："), central);
    m_targetEdit = new QLineEdit(central);
    m_targetEdit->setPlaceholderText(QStringLiteral("默认当前目录，可直接编辑或浏览选择"));
    m_targetEdit->setClearButtonEnabled(true);
    m_browseTargetButton = new QPushButton(QStringLiteral("浏览…"), central);
    targetRow->addWidget(m_targetLabel);
    targetRow->addWidget(m_targetEdit, 1);
    targetRow->addWidget(m_browseTargetButton);
    rootLayout->addLayout(targetRow);

    // 高频操作
    auto *actionRow = new QHBoxLayout();
    actionRow->setSpacing(8);
    m_selectButton = new QPushButton(QStringLiteral("选择文件…"), central);
    m_selectButton->setObjectName(QStringLiteral("primaryButton"));
    m_selectButton->setToolTip(QStringLiteral("打开 ZIP / 7Z / RAR 等压缩包"));
    m_extractHereButton = new QPushButton(QStringLiteral("解压到此处"), central);
    m_extractHereButton->setObjectName(QStringLiteral("primaryButton"));
    m_extractHereButton->setToolTip(QStringLiteral("一键解压到上方目标目录"));
    m_extractToButton = new QPushButton(QStringLiteral("解压到…"), central);
    m_extractToButton->setToolTip(QStringLiteral("选择目标目录并立即解压"));
    actionRow->addWidget(m_selectButton);
    actionRow->addWidget(m_extractHereButton);
    actionRow->addWidget(m_extractToButton);
    actionRow->addStretch(1);
    rootLayout->addLayout(actionRow);

    // 中部：文件列表 / 待压缩列表 / 任务日志
    m_tabs = new QTabWidget(central);
    m_archivePage = new QWidget(m_tabs);
    auto *archiveLayout = new QVBoxLayout(m_archivePage);
    archiveLayout->setContentsMargins(8, 8, 8, 8);
    archiveLayout->setSpacing(6);
    m_archiveInfoLabel = new QLabel(
        QStringLiteral("尚未打开压缩包。拖入压缩包，或点击“选择文件…”。"), m_archivePage);
    m_archiveInfoLabel->setWordWrap(true);
    m_archiveInfoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_archiveTree = new QTreeWidget(m_archivePage);
    m_archiveTree->setColumnCount(3);
    m_archiveTree->setHeaderLabels({QStringLiteral("名称"), QStringLiteral("大小"),
                                    QStringLiteral("修改时间")});
    m_archiveTree->setRootIsDecorated(false);
    m_archiveTree->setAlternatingRowColors(true);
    m_archiveTree->setUniformRowHeights(true);
    m_archiveTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_archiveTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_archiveTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_archiveTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    archiveLayout->addWidget(m_archiveInfoLabel);
    archiveLayout->addWidget(m_archiveTree, 1);
    m_tabs->addTab(m_archivePage, QStringLiteral("压缩包内容"));

    m_sourcesPage = new QWidget(m_tabs);
    auto *sourcesLayout = new QVBoxLayout(m_sourcesPage);
    sourcesLayout->setContentsMargins(8, 8, 8, 8);
    sourcesLayout->setSpacing(8);
    auto *sourceActions = new QHBoxLayout();
    sourceActions->setSpacing(8);
    m_addFilesButton = new QPushButton(QStringLiteral("添加文件…"), m_sourcesPage);
    m_addFolderButton = new QPushButton(QStringLiteral("添加文件夹…"), m_sourcesPage);
    m_removeSourceButton = new QPushButton(QStringLiteral("移除选中"), m_sourcesPage);
    m_clearSourcesButton = new QPushButton(QStringLiteral("清空"), m_sourcesPage);
    m_createZipButton = new QPushButton(QStringLiteral("压缩为 ZIP…"), m_sourcesPage);
    m_createZipButton->setObjectName(QStringLiteral("primaryButton"));
    m_createZipButton->setToolTip(
        QStringLiteral("0.0.1 仅创建 ZIP；RAR/7Z 用于解压，不支持创建"));
    sourceActions->addWidget(m_addFilesButton);
    sourceActions->addWidget(m_addFolderButton);
    sourceActions->addWidget(m_removeSourceButton);
    sourceActions->addWidget(m_clearSourcesButton);
    sourceActions->addStretch(1);
    sourceActions->addWidget(m_createZipButton);
    m_sourcesTree = new QTreeWidget(m_sourcesPage);
    m_sourcesTree->setColumnCount(3);
    m_sourcesTree->setHeaderLabels({QStringLiteral("名称"), QStringLiteral("类型"),
                                    QStringLiteral("路径")});
    m_sourcesTree->setRootIsDecorated(false);
    m_sourcesTree->setAlternatingRowColors(true);
    m_sourcesTree->setUniformRowHeights(true);
    m_sourcesTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_sourcesTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_sourcesTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_sourcesTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    auto *sourceHint = new QLabel(
        QStringLiteral("添加要打包的文件或文件夹，点击右侧“压缩为 ZIP…”选择输出位置。"),
        m_sourcesPage);
    sourceHint->setWordWrap(true);
    sourcesLayout->addLayout(sourceActions);
    sourcesLayout->addWidget(sourceHint);
    sourcesLayout->addWidget(m_sourcesTree, 1);
    m_tabs->addTab(m_sourcesPage, QStringLiteral("压缩任务"));

    m_logPage = new QWidget(m_tabs);
    auto *logLayout = new QVBoxLayout(m_logPage);
    logLayout->setContentsMargins(8, 8, 8, 8);
    m_logView = new QPlainTextEdit(m_logPage);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(3000);
    m_logView->setPlaceholderText(QStringLiteral("任务日志将显示在这里。"));
    logLayout->addWidget(m_logView);
    m_tabs->addTab(m_logPage, QStringLiteral("运行日志"));

    rootLayout->addWidget(m_tabs, 1);

    // 底部进度区
    auto *progressRow = new QHBoxLayout();
    progressRow->setSpacing(10);
    m_progressLabel = new QLabel(QStringLiteral("就绪"), central);
    m_progressLabel->setMinimumWidth(150);
    m_progressBar = new QProgressBar(central);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_cancelButton = new QPushButton(QStringLiteral("取消任务"), central);
    m_cancelButton->setEnabled(false);
    progressRow->addWidget(m_progressLabel);
    progressRow->addWidget(m_progressBar, 1);
    progressRow->addWidget(m_cancelButton);
    rootLayout->addLayout(progressRow);

    setCentralWidget(central);

    // 状态栏
    statusBar()->showMessage(QStringLiteral("就绪"));
    auto *versionLabel = new QLabel(QStringLiteral("StarZip 0.0.1"), this);
    statusBar()->addPermanentWidget(versionLabel);

    connect(m_selectButton, &QPushButton::clicked, this, &MainWindow::chooseArchive);
    connect(m_browseTargetButton, &QPushButton::clicked, this, &MainWindow::chooseExtractTarget);
    connect(m_extractHereButton, &QPushButton::clicked, this, &MainWindow::extractHere);
    connect(m_extractToButton, &QPushButton::clicked, this, &MainWindow::extractToChosen);
    connect(m_cancelButton, &QPushButton::clicked, this, &MainWindow::cancelCurrentTask);
    connect(m_addFilesButton, &QPushButton::clicked, this, &MainWindow::addFiles);
    connect(m_addFolderButton, &QPushButton::clicked, this, &MainWindow::addFolder);
    connect(m_removeSourceButton, &QPushButton::clicked, this, &MainWindow::removeSelectedSources);
    connect(m_clearSourcesButton, &QPushButton::clicked, this, &MainWindow::clearSources);
    connect(m_createZipButton, &QPushButton::clicked, this, &MainWindow::createZip);
    connect(m_sourcesTree, &QTreeWidget::itemSelectionChanged, this, &MainWindow::updateActions);

    setStyleSheet(QStringLiteral(R"(
        QMainWindow { background: #f5f7fb; }
        QLabel { color: #26324a; }
        QFrame#dropZone {
            background: #ffffff;
            border: 2px dashed #8fa2ff;
            border-radius: 12px;
        }
        QFrame#dropZone[dragActive="true"] {
            background: #eef3ff;
            border: 2px solid #4f6bff;
        }
        QLabel#dropTitle { font-size: 20px; font-weight: 600; color: #1f2a44; }
        QLabel#dropHint { color: #6f7890; }
        QPushButton {
            padding: 6px 14px;
            border: 1px solid #cfd5e3;
            border-radius: 6px;
            background: #ffffff;
            color: #26324a;
        }
        QPushButton:hover { background: #f0f4ff; border-color: #8fa2ff; }
        QPushButton:pressed { background: #e3eaff; }
        QPushButton:disabled { color: #9aa3b5; background: #f3f4f7; border-color: #e2e5ed; }
        QPushButton#primaryButton {
            background: #4f6bff; color: #ffffff; border-color: #4f6bff;
        }
        QPushButton#primaryButton:hover { background: #3e58e8; border-color: #3e58e8; }
        QPushButton#primaryButton:disabled { background: #b8c2e8; border-color: #b8c2e8; }
        QLineEdit {
            padding: 6px 8px; border: 1px solid #cfd5e3;
            border-radius: 6px; background: #ffffff;
        }
        QLineEdit:focus { border-color: #4f6bff; }
        QTabWidget::pane {
            border: 1px solid #dfe3ec;
            border-radius: 8px;
            background: #ffffff;
        }
        QTabBar::tab {
            padding: 7px 16px;
            margin-right: 2px;
            color: #5b6478;
            background: transparent;
        }
        QTabBar::tab:selected { color: #3448c5; font-weight: 600; }
        QTreeWidget, QPlainTextEdit {
            border: none;
            background: #ffffff;
            alternate-background-color: #fafbff;
        }
        QHeaderView::section {
            background: #f2f5fb;
            border: none;
            border-bottom: 1px solid #dfe3ec;
            padding: 6px;
            color: #4b556b;
        }
        QProgressBar {
            border: 1px solid #dfe3ec;
            border-radius: 6px;
            background: #ffffff;
            text-align: center;
            min-height: 20px;
        }
        QProgressBar::chunk { background: #4f6bff; border-radius: 5px; }
        QStatusBar { background: #eef1f7; color: #4b556b; }
    )"));

    refreshSourcesSummary();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_busy || !event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }
    for (const QUrl &url : event->mimeData()->urls()) {
        if (urlIsLocalFile(url)) {
            event->acceptProposedAction();
            setDropZoneActive(true);
            return;
        }
    }
    event->ignore();
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event)
{
    if (m_busy || !event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }
    for (const QUrl &url : event->mimeData()->urls()) {
        if (urlIsLocalFile(url)) {
            event->acceptProposedAction();
            setDropZoneActive(true);
            return;
        }
    }
    event->ignore();
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent *event)
{
    setDropZoneActive(false);
    QMainWindow::dragLeaveEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    setDropZoneActive(false);
    if (m_busy) {
        event->ignore();
        return;
    }

    QStringList localFiles;
    if (event->mimeData()->hasUrls()) {
        for (const QUrl &url : event->mimeData()->urls()) {
            if (urlIsLocalFile(url))
                localFiles.append(url.toLocalFile());
        }
    }
    if (localFiles.isEmpty()) {
        event->ignore();
        return;
    }

    event->acceptProposedAction();
    if (localFiles.size() > 1) {
        statusBar()->showMessage(QStringLiteral("一次只处理一个压缩包，已打开第一个文件。"), 6000);
        appendLog(QStringLiteral("拖入了多个文件，仅打开第一个：%1")
                      .arg(QDir::toNativeSeparators(localFiles.first())));
    }
    openArchive(localFiles.first());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_busy) {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, QStringLiteral("任务进行中"),
            QStringLiteral("压缩/解压任务仍在进行。确定取消任务并退出 StarZip 吗？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        m_closing = true;
        if (m_worker)
            m_worker->requestCancel();
    }

    if (m_thread && m_thread->isRunning()) {
        if (m_busy)
            m_progressLabel->setText(QStringLiteral("正在取消任务…"));
        m_thread->quit();
        if (!m_thread->wait(15000)) {
            QMessageBox::warning(this, QStringLiteral("暂不能退出"),
                                 QStringLiteral("后台任务仍未停止，请稍候再关闭窗口。"));
            m_closing = false;
            event->ignore();
            return;
        }
    }
    event->accept();
}

void MainWindow::chooseArchive()
{
    if (m_busy) {
        statusBar()->showMessage(QStringLiteral("当前有任务正在进行，请稍候。"), 4000);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择压缩包"), QDir::currentPath(),
        QStringLiteral("压缩包 (*.zip *.7z *.rar *.tar *.gz *.tgz *.bz2 *.xz *.lzma *.cab *.iso);;"
                       "所有文件 (*.*)"));
    if (!path.isEmpty())
        openArchive(path);
}

void MainWindow::openArchive(const QString &path)
{
    if (m_busy) {
        statusBar()->showMessage(QStringLiteral("当前有任务正在进行，请稍候。"), 4000);
        return;
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        showError(QStringLiteral("无法打开压缩包"),
                  QStringLiteral("文件不存在或不是普通文件：\n%1")
                      .arg(QDir::toNativeSeparators(path)));
        return;
    }

    m_archivePath = info.absoluteFilePath();
    m_archiveLabel->setText(QDir::toNativeSeparators(m_archivePath));
    m_archiveLabel->setToolTip(QDir::toNativeSeparators(m_archivePath));
    m_archiveInfoLabel->setText(QStringLiteral("正在识别压缩包格式…"));
    m_archiveTree->clear();
    m_entries.clear();
    m_formatName.clear();
    m_tabs->setCurrentWidget(m_archivePage);
    setWindowTitle(QStringLiteral("StarZip 0.0.1 — %1").arg(archiveDisplayName(m_archivePath)));
    appendLog(QStringLiteral("打开压缩包：%1").arg(QDir::toNativeSeparators(m_archivePath)));

    const QString archivePath = m_archivePath;
    if (!beginJob(ArchiveWorker::List, QStringLiteral("正在识别压缩包…"), true,
                  [archivePath](ArchiveWorker *worker) { worker->listArchive(archivePath); })) {
        return;
    }
}

void MainWindow::chooseExtractTarget()
{
    if (m_busy)
        return;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择解压目标目录"),
        m_targetEdit->text().isEmpty() ? QDir::currentPath() : m_targetEdit->text());
    if (dir.isEmpty())
        return;
    m_extractTarget = QDir(dir).absolutePath();
    m_targetEdit->setText(QDir::toNativeSeparators(m_extractTarget));
    statusBar()->showMessage(QStringLiteral("解压目标：%1")
                                 .arg(QDir::toNativeSeparators(m_extractTarget)),
                             6000);
}

void MainWindow::extractHere()
{
    if (m_archivePath.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("尚未打开压缩包"),
                                 QStringLiteral("请先拖入压缩包，或点击“选择文件…”。"));
        return;
    }
    QString destination = m_targetEdit->text().trimmed();
    if (destination.isEmpty())
        destination = QDir::currentPath();
    startExtraction(destination);
}

void MainWindow::extractToChosen()
{
    if (m_archivePath.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("尚未打开压缩包"),
                                 QStringLiteral("请先拖入压缩包，或点击“选择文件…”。"));
        return;
    }
    if (m_busy)
        return;
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择解压目标目录"),
        m_targetEdit->text().isEmpty() ? QDir::currentPath() : m_targetEdit->text());
    if (dir.isEmpty())
        return;
    m_extractTarget = QDir(dir).absolutePath();
    m_targetEdit->setText(QDir::toNativeSeparators(m_extractTarget));
    startExtraction(m_extractTarget);
}

void MainWindow::startExtraction(const QString &destination)
{
    if (m_archivePath.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("尚未打开压缩包"),
                                 QStringLiteral("请先打开一个压缩包。"));
        return;
    }
    if (m_busy) {
        statusBar()->showMessage(QStringLiteral("当前有任务正在进行，请稍候。"), 4000);
        return;
    }

    QString target = destination.trimmed();
    if (target.isEmpty())
        target = QDir::currentPath();
    const QFileInfo targetInfo(target);
    if (targetInfo.exists() && !targetInfo.isDir()) {
        showError(QStringLiteral("解压目标无效"),
                  QStringLiteral("目标路径已存在且不是文件夹：\n%1")
                      .arg(QDir::toNativeSeparators(target)));
        return;
    }

    m_extractTarget = QDir(target).absolutePath();
    m_targetEdit->setText(QDir::toNativeSeparators(m_extractTarget));
    const QString archivePath = m_archivePath;
    const QString destinationDir = m_extractTarget;

    appendLog(QStringLiteral("开始解压：%1 → %2")
                  .arg(QDir::toNativeSeparators(archivePath),
                       QDir::toNativeSeparators(destinationDir)));

    beginJob(ArchiveWorker::Extract, QStringLiteral("正在解压…"), false,
             [archivePath, destinationDir](ArchiveWorker *worker) {
                 worker->extractArchive(archivePath, destinationDir);
             });
}

void MainWindow::addFiles()
{
    if (m_busy)
        return;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择要压缩的文件"),
        m_sourcesTree->topLevelItemCount() > 0
            ? QFileInfo(m_sourcesTree->topLevelItem(0)
                            ->data(0, Qt::UserRole)
                            .toString())
                  .absolutePath()
            : QDir::homePath(),
        QStringLiteral("所有文件 (*.*)"));
    for (const QString &file : files)
        addSourcePath(file);
    refreshSourcesSummary();
    if (!files.isEmpty())
        m_tabs->setCurrentWidget(m_sourcesPage);
}

void MainWindow::addFolder()
{
    if (m_busy)
        return;
    const QString folder = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择要压缩的文件夹"),
        m_sourcesTree->topLevelItemCount() > 0
            ? QFileInfo(m_sourcesTree->topLevelItem(0)
                            ->data(0, Qt::UserRole)
                            .toString())
                  .absolutePath()
            : QDir::homePath());
    if (folder.isEmpty())
        return;
    addSourcePath(folder);
    refreshSourcesSummary();
    m_tabs->setCurrentWidget(m_sourcesPage);
}

void MainWindow::addSourcePath(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        showError(QStringLiteral("无法添加"), QStringLiteral("路径不存在：\n%1")
                                                .arg(QDir::toNativeSeparators(path)));
        return;
    }
    const QString absolute = info.absoluteFilePath();
    for (int i = 0; i < m_sourcesTree->topLevelItemCount(); ++i) {
        const QString existing =
            m_sourcesTree->topLevelItem(i)->data(0, Qt::UserRole).toString();
        if (existing.compare(absolute, Qt::CaseInsensitive) == 0)
            return;
    }

    auto *item = new QTreeWidgetItem(m_sourcesTree);
    item->setText(0, info.fileName().isEmpty() ? absolute : info.fileName());
    item->setText(1, info.isDir() ? QStringLiteral("文件夹") : QStringLiteral("文件"));
    item->setText(2, QDir::toNativeSeparators(absolute));
    item->setData(0, Qt::UserRole, absolute);
    item->setToolTip(2, QDir::toNativeSeparators(absolute));
    if (info.isDir())
        item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
    else
        item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
}

void MainWindow::removeSelectedSources()
{
    if (m_busy)
        return;
    const QList<QTreeWidgetItem *> selected = m_sourcesTree->selectedItems();
    for (QTreeWidgetItem *item : selected)
        delete item;
    refreshSourcesSummary();
}

void MainWindow::clearSources()
{
    if (m_busy)
        return;
    m_sourcesTree->clear();
    refreshSourcesSummary();
}

void MainWindow::createZip()
{
    if (m_busy) {
        statusBar()->showMessage(QStringLiteral("当前有任务正在进行，请稍候。"), 4000);
        return;
    }
    const QStringList sources = selectedSources();
    if (sources.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("没有待压缩内容"),
                                 QStringLiteral("请先添加要压缩的文件或文件夹。"));
        return;
    }

    const QFileInfo first(sources.first());
    const QString baseName = sources.size() == 1 && !first.completeBaseName().isEmpty()
                                 ? first.completeBaseName()
                                 : QStringLiteral("StarZip_%1")
                                       .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    QString suggested = QDir(first.absolutePath()).filePath(baseName + QStringLiteral(".zip"));

    QString zipPath = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存 ZIP 压缩包"), suggested,
        QStringLiteral("ZIP 压缩包 (*.zip)"));
    if (zipPath.isEmpty())
        return;
    if (!zipPath.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive))
        zipPath += QStringLiteral(".zip");

    appendLog(QStringLiteral("开始压缩 %1 个源项目 → %2")
                  .arg(sources.size())
                  .arg(QDir::toNativeSeparators(zipPath)));

    beginJob(ArchiveWorker::Compress, QStringLiteral("正在准备压缩…"), false,
             [sources, zipPath](ArchiveWorker *worker) {
                 worker->compressToZip(sources, zipPath);
             });
}

bool MainWindow::beginJob(ArchiveWorker::Operation operation, const QString &busyText,
                          bool indeterminate,
                          const std::function<void(ArchiveWorker *)> &task)
{
    if (m_busy)
        return false;
    if (m_thread && m_thread->isRunning()) {
        statusBar()->showMessage(QStringLiteral("上一任务的线程正在收尾，请稍候再试。"), 4000);
        return false;
    }

    m_busy = true;
    m_currentOperation = operation;
    m_progressBar->setRange(0, indeterminate ? 0 : 100);
    m_progressBar->setValue(0);
    m_progressLabel->setText(busyText);
    statusBar()->showMessage(busyText);
    updateActions();

    auto *thread = new QThread(this);
    auto *worker = new ArchiveWorker();
    worker->moveToThread(thread);
    m_thread = thread;
    m_worker = worker;

    // started 在 worker 所在线程触发，确保耗时的 libarchive 调用完全不阻塞 UI 线程。
    connect(thread, &QThread::started, worker, [worker, task]() { task(worker); });
    connect(worker, &ArchiveWorker::progress, this, &MainWindow::onWorkerProgress,
            Qt::QueuedConnection);
    connect(worker, &ArchiveWorker::listed, this, &MainWindow::onArchiveListed,
            Qt::QueuedConnection);
    connect(worker, &ArchiveWorker::finished, this, &MainWindow::onWorkerFinished,
            Qt::QueuedConnection);
    connect(worker, &ArchiveWorker::finished, thread, &QThread::quit);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);

    thread->start();
    return true;
}

void MainWindow::onWorkerProgress(int percent, const QString &detail)
{
    if (!m_busy)
        return;
    if (m_progressBar->maximum() == 0)
        m_progressBar->setRange(0, 100);
    m_progressBar->setValue(qBound(0, percent, 100));
    m_progressLabel->setText(detail);
    statusBar()->showMessage(detail);
}

void MainWindow::onArchiveListed(const QVector<ArchiveEntry> &entries,
                                const QString &formatName, qint64 totalBytes, int entryCount,
                                bool truncated)
{
    m_entries = entries;
    m_formatName = formatName.isEmpty() ? QStringLiteral("未知格式") : formatName;
    fillArchiveTree(entries);
    updateArchiveHeader(m_formatName, totalBytes, entryCount, truncated);
    m_tabs->setCurrentWidget(m_archivePage);

    appendLog(QStringLiteral("识别完成：格式 %1，%2 个条目，解压后 %3%4")
                  .arg(m_formatName)
                  .arg(entryCount)
                  .arg(humanSize(totalBytes))
                  .arg(truncated ? QStringLiteral("（列表已截断）") : QString()));
}

void MainWindow::onWorkerFinished(int operation, bool success, bool canceled,
                                  const QString &message, int errorCode,
                                  const QString &outputPath)
{
    Q_UNUSED(outputPath)
    m_busy = false;
    m_worker.clear();
    m_progressBar->setRange(0, 100);
    m_progressLabel->setText(QStringLiteral("就绪"));
    m_cancelButton->setEnabled(false);

    if (canceled) {
        m_progressBar->setValue(0);
        m_progressLabel->setText(QStringLiteral("已取消"));
        if (operation == ArchiveWorker::List) {
            m_archivePath.clear();
            m_archiveLabel->setText(QStringLiteral("未选择"));
            m_archiveInfoLabel->setText(QStringLiteral("尚未打开压缩包。"));
            setWindowTitle(QStringLiteral("StarZip 0.0.1"));
        }
        statusBar()->showMessage(QStringLiteral("操作已取消。"), 6000);
        appendLog(QStringLiteral("操作已取消。"));
        updateActions();
        return;
    }

    if (!success) {
        m_progressBar->setValue(0);
        if (operation == ArchiveWorker::List) {
            m_archivePath.clear();
            m_archiveLabel->setText(QStringLiteral("未选择"));
            m_archiveInfoLabel->setText(QStringLiteral("无法读取该文件。"));
            setWindowTitle(QStringLiteral("StarZip 0.0.1"));
        }
        if (errorCode != 0)
            appendLog(QStringLiteral("错误码：%1").arg(errorCode));
        showError(operationLabel(operation) + QStringLiteral("失败"), message);
        refreshSourcesSummary();
        updateActions();
        return;
    }

    if (operation == ArchiveWorker::List) {
        // 列表成功提示由 onArchiveListed 与状态栏完成，不额外弹出模态框。
        statusBar()->showMessage(message, 8000);
        appendLog(message);
    } else {
        m_progressBar->setValue(100);
        m_progressLabel->setText(QStringLiteral("完成"));
        appendLog(message);
        showSuccess(operationLabel(operation) + QStringLiteral("成功"), message);
    }
    updateActions();
}

void MainWindow::finishJobUi()
{
    m_cancelButton->setEnabled(false);
    updateActions();
}

void MainWindow::updateActions()
{
    const bool idle = !m_busy;
    m_selectButton->setEnabled(idle);
    m_browseTargetButton->setEnabled(idle);
    m_targetEdit->setEnabled(idle);
    m_extractHereButton->setEnabled(idle && !m_archivePath.isEmpty());
    m_extractToButton->setEnabled(idle && !m_archivePath.isEmpty());
    m_cancelButton->setEnabled(m_busy);

    const bool hasSources = m_sourcesTree->topLevelItemCount() > 0;
    m_addFilesButton->setEnabled(idle);
    m_addFolderButton->setEnabled(idle);
    m_removeSourceButton->setEnabled(idle && !m_sourcesTree->selectedItems().isEmpty());
    m_clearSourcesButton->setEnabled(idle && hasSources);
    m_createZipButton->setEnabled(idle && hasSources);
}

void MainWindow::setDropZoneActive(bool active)
{
    if (m_dropZone->property("dragActive").toBool() == active)
        return;
    m_dropZone->setProperty("dragActive", active);
    m_dropZone->style()->unpolish(m_dropZone);
    m_dropZone->style()->polish(m_dropZone);
}

void MainWindow::appendLog(const QString &text)
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    m_logView->appendPlainText(QStringLiteral("[%1] %2").arg(stamp, text));
}

void MainWindow::showError(const QString &title, const QString &message)
{
    appendLog(QStringLiteral("%1：%2").arg(title, message));
    statusBar()->showMessage(QStringLiteral("%1：%2").arg(title, firstLine(message)), 15000);
    QMessageBox box(this);
    box.setIcon(QMessageBox::Critical);
    box.setWindowTitle(title);
    box.setText(title);
    box.setInformativeText(message);
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
}

void MainWindow::showSuccess(const QString &title, const QString &message)
{
    statusBar()->showMessage(QStringLiteral("%1：%2").arg(title, firstLine(message)), 10000);
    QMessageBox box(this);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(title);
    box.setText(title);
    box.setInformativeText(message);
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
}

QStringList MainWindow::selectedSources() const
{
    QStringList paths;
    for (int i = 0; i < m_sourcesTree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *item = m_sourcesTree->topLevelItem(i);
        const QString path = item->data(0, Qt::UserRole).toString();
        if (!path.isEmpty())
            paths.append(path);
    }
    return paths;
}

void MainWindow::refreshSourcesSummary()
{
    const int count = m_sourcesTree->topLevelItemCount();
    m_createZipButton->setText(count > 0
                                   ? QStringLiteral("压缩为 ZIP（%1 项）…").arg(count)
                                   : QStringLiteral("压缩为 ZIP…"));
    updateActions();
}

void MainWindow::fillArchiveTree(const QVector<ArchiveEntry> &entries)
{
    m_archiveTree->setUpdatesEnabled(false);
    m_archiveTree->clear();

    QVector<ArchiveEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const ArchiveEntry &a, const ArchiveEntry &b) {
        return a.path.compare(b.path, Qt::CaseInsensitive) < 0;
    });

    for (const ArchiveEntry &entry : sorted) {
        auto *item = new QTreeWidgetItem(m_archiveTree);
        QString name = entry.path;
        if (entry.encrypted)
            name += QStringLiteral("  [加密]");
        item->setText(0, name);
        item->setIcon(0, style()->standardIcon(entry.isDir ? QStyle::SP_DirIcon
                                                          : QStyle::SP_FileIcon));
        item->setText(1, entry.isDir ? QStringLiteral("文件夹") : humanSize(entry.size));
        item->setText(2, entry.modified.isValid()
                             ? entry.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                             : QStringLiteral("—"));
        if (entry.encrypted)
            item->setForeground(0, QBrush(QColor(QStringLiteral("#b45309"))));
        item->setToolTip(0, entry.path);
    }

    m_archiveTree->setUpdatesEnabled(true);
}

void MainWindow::updateArchiveHeader(const QString &formatName, qint64 totalBytes,
                                     int entryCount, bool truncated)
{
    QString text = QStringLiteral("格式：%1    条目：%2    解压后大小：%3")
                       .arg(formatName)
                       .arg(entryCount)
                       .arg(humanSize(totalBytes));
    if (truncated)
        text += QStringLiteral("    列表仅显示前 %1 项").arg(20000);
    if (formatName.contains(QStringLiteral("RAR"), Qt::CaseInsensitive)) {
        text += QStringLiteral("\nRAR/RAR5 仅支持解压；加密、分卷或不受支持的压缩算法会明确报错，"
                               "本版本不支持创建 RAR。");
    }
    m_archiveInfoLabel->setText(text);
}

void MainWindow::cancelCurrentTask()
{
    if (!m_busy || !m_worker)
        return;
    m_worker->requestCancel();
    m_cancelButton->setEnabled(false);
    m_progressLabel->setText(QStringLiteral("正在取消任务…"));
    statusBar()->showMessage(QStringLiteral("正在取消后台任务…"));
    appendLog(QStringLiteral("已请求取消当前任务。"));
}

QString MainWindow::humanSize(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("—");
    const double value = static_cast<double>(bytes);
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024LL * 1024)
        return QStringLiteral("%1 KiB").arg(QString::number(value / 1024.0, 'f', 1));
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MiB").arg(QString::number(value / (1024.0 * 1024.0), 'f', 1));
    return QStringLiteral("%1 GiB").arg(QString::number(value / (1024.0 * 1024.0 * 1024.0),
                                                        'f', 2));
}

QString MainWindow::operationLabel(int operation)
{
    switch (operation) {
    case ArchiveWorker::List:
        return QStringLiteral("打开压缩包");
    case ArchiveWorker::Extract:
        return QStringLiteral("解压");
    case ArchiveWorker::Compress:
        return QStringLiteral("压缩");
    default:
        return QStringLiteral("操作");
    }
}

QString MainWindow::firstLine(const QString &text)
{
    return text.section(QLatin1Char('\n'), 0, 0).left(220);
}
