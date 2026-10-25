#include "MainWindow.h"

#include "ExternalEngine.h"
#include "ShellIntegration.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

bool urlIsLocalPath(const QUrl &url)
{
    if (!url.isLocalFile())
        return false;
    const QFileInfo info(url.toLocalFile());
    return info.exists() && (info.isFile() || info.isDir());
}

QIcon makeAppIcon()
{
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient gradient(0, 0, 64, 64);
    gradient.setColorAt(0.0, QColor(QStringLiteral("#2f6fed")));
    gradient.setColorAt(1.0, QColor(QStringLiteral("#1d3fa8")));
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawRoundedRect(QRectF(5, 5, 54, 54), 12, 12);

    painter.setPen(QPen(QColor(255, 255, 255, 235), 3.2, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(25, 15), QPointF(25, 49));
    painter.drawLine(QPointF(39, 15), QPointF(39, 49));
    for (int y = 17; y <= 45; y += 8) {
        painter.drawLine(QPointF(25, y), QPointF(32, y));
        painter.drawLine(QPointF(32, y + 3), QPointF(39, y + 3));
    }
    painter.setPen(QPen(QColor(255, 255, 255, 235), 2.4, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(QRectF(27, 24, 10, 16), 0, 180 * 16);
    return QIcon(pixmap);
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    qRegisterMetaType<ArchiveEntry>("ArchiveEntry");
    qRegisterMetaType<QVector<ArchiveEntry>>("QVector<ArchiveEntry>");

    setupUi();

    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("windowGeometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
        // 旧版本保存过 1120×780 的大窗口；启动时收敛到紧凑尺寸。
        if (isMaximized() || isFullScreen())
            setWindowState(Qt::WindowNoState);
        if (width() > 980 || height() > 700)
            resize(900, 600);
    }

    m_extractTarget = QDir::currentPath();
    m_targetEdit->setText(QDir::toNativeSeparators(m_extractTarget));
    m_targetEdit->setToolTip(QStringLiteral("默认解压到 StarZip 启动时的当前目录：%1")
                                 .arg(QDir::toNativeSeparators(m_extractTarget)));

#ifdef Q_OS_WIN
    // 首次运行自动注册当前用户的右键菜单与“打开方式”条目，无需管理员权限。
    if (ShellIntegration::supported()
        && !settings.value(QStringLiteral("shellIntegration/autoInstalled"), false).toBool()) {
        QString shellError;
        if (ShellIntegration::install(&shellError)) {
            settings.setValue(QStringLiteral("shellIntegration/autoInstalled"), true);
            settings.sync();
            appendLog(QStringLiteral("已添加资源管理器右键菜单：解压到当前文件夹 / 解压到… / "
                                     "使用 StarZip 压缩。"));
        } else {
            appendLog(QStringLiteral("添加资源管理器右键菜单失败：%1").arg(shellError));
        }
    }
#endif

    appendLog(QStringLiteral("StarZip 0.0.3 已启动。"));
    appendLog(QStringLiteral("读取：ZIP / 7Z / RAR / RAR5 / TAR / GZ / XZ / CAB / ISO；"
                             "创建：ZIP / 7Z / TAR / TAR.GZ / TAR.XZ。"));
    appendLog(QStringLiteral("密码：ZIP 支持 AES-256 写入；加密 ZIP / 7Z / RAR 可输入密码解压。"));
    appendLog(ExternalEngine::available()
                  ? QStringLiteral("7-Zip 引擎：%1（支持 UDF ISO 与加密 7Z）")
                        .arg(ExternalEngine::executablePath())
                  : QStringLiteral("7-Zip 引擎未找到：UDF ISO 与加密 7Z 将不可用。"));
    rebuildRecentMenu();
    statusBar()->showMessage(QStringLiteral("就绪 — 拖入压缩包，或点击“打开压缩包…”"));
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
    setWindowTitle(QStringLiteral("StarZip 0.0.3"));
    setWindowIcon(makeAppIcon());
    resize(900, 600);
    setMinimumSize(760, 500);
    setAcceptDrops(true);

    setupActions();
    setupMenus();
    setupToolBar();

    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("appRoot"));
    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(10, 8, 10, 8);
    rootLayout->setSpacing(8);

    // 路径卡片：打开压缩包 + 解压目标
    auto *pathCard = new QFrame(central);
    pathCard->setObjectName(QStringLiteral("pathCard"));
    auto *pathLayout = new QVBoxLayout(pathCard);
    pathLayout->setContentsMargins(12, 9, 12, 9);
    pathLayout->setSpacing(7);

    auto *archiveRow = new QHBoxLayout();
    archiveRow->setSpacing(8);
    auto *archiveCaption = new QLabel(QStringLiteral("压缩包"), pathCard);
    archiveCaption->setObjectName(QStringLiteral("fieldLabel"));
    archiveCaption->setMinimumWidth(48);
    m_archiveLabel = new QLabel(QStringLiteral("未选择"), pathCard);
    m_archiveLabel->setObjectName(QStringLiteral("archivePathLabel"));
    m_archiveLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_archiveLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_archiveLabel->setToolTip(QStringLiteral("当前打开的压缩包"));
    m_openButton = new QPushButton(QStringLiteral("打开…"), pathCard);
    m_openButton->setToolTip(QStringLiteral("从本地选择 ZIP / 7Z / RAR 等压缩包"));
    archiveRow->addWidget(archiveCaption);
    archiveRow->addWidget(m_archiveLabel, 1);
    archiveRow->addWidget(m_openButton);
    pathLayout->addLayout(archiveRow);

    auto *targetRow = new QHBoxLayout();
    targetRow->setSpacing(8);
    m_targetLabel = new QLabel(QStringLiteral("解压到"), pathCard);
    m_targetLabel->setObjectName(QStringLiteral("fieldLabel"));
    m_targetLabel->setMinimumWidth(48);
    m_targetEdit = new QLineEdit(pathCard);
    m_targetEdit->setPlaceholderText(QStringLiteral("默认当前目录，可直接编辑或浏览选择"));
    m_targetEdit->setClearButtonEnabled(true);
    m_browseTargetButton = new QPushButton(QStringLiteral("浏览…"), pathCard);
    m_extractHereButton = new QPushButton(QStringLiteral("解压到此处"), pathCard);
    m_extractHereButton->setObjectName(QStringLiteral("primaryButton"));
    m_extractHereButton->setToolTip(QStringLiteral("一键解压全部条目到上方目标目录"));
    m_extractToButton = new QPushButton(QStringLiteral("解压到…"), pathCard);
    m_extractToButton->setToolTip(QStringLiteral("选择目标目录并立即解压全部条目"));
    targetRow->addWidget(m_targetLabel);
    targetRow->addWidget(m_targetEdit, 1);
    targetRow->addWidget(m_browseTargetButton);
    targetRow->addWidget(m_extractHereButton);
    targetRow->addWidget(m_extractToButton);
    pathLayout->addLayout(targetRow);
    rootLayout->addWidget(pathCard);

    m_tabs = new QTabWidget(central);
    m_archivePage = createArchivePage();
    m_compressPage = createCompressPage();
    m_logPage = createLogPage();
    m_tabs->addTab(m_archivePage, QStringLiteral("压缩包内容"));
    m_tabs->addTab(m_compressPage, QStringLiteral("压缩任务"));
    m_tabs->addTab(m_logPage, QStringLiteral("运行日志"));
    rootLayout->addWidget(m_tabs, 1);

    auto *progressCard = new QFrame(central);
    progressCard->setObjectName(QStringLiteral("progressCard"));
    auto *progressRow = new QHBoxLayout(progressCard);
    progressRow->setContentsMargins(10, 6, 10, 6);
    progressRow->setSpacing(10);
    m_progressLabel = new QLabel(QStringLiteral("就绪"), progressCard);
    m_progressLabel->setMinimumWidth(230);
    m_progressBar = new QProgressBar(progressCard);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_cancelButton = new QPushButton(QStringLiteral("取消"), progressCard);
    m_cancelButton->setEnabled(false);
    progressRow->addWidget(m_progressLabel);
    progressRow->addWidget(m_progressBar, 1);
    progressRow->addWidget(m_cancelButton);
    rootLayout->addWidget(progressCard);

    setCentralWidget(central);

    statusBar()->showMessage(QStringLiteral("就绪"));
    m_statusFormatLabel = new QLabel(QStringLiteral("格式：—"), this);
    m_statusCountLabel = new QLabel(QStringLiteral("条目：—"), this);
    auto *versionLabel = new QLabel(QStringLiteral("StarZip 0.0.3"), this);
    statusBar()->addPermanentWidget(m_statusFormatLabel);
    statusBar()->addPermanentWidget(m_statusCountLabel);
    statusBar()->addPermanentWidget(versionLabel);

    connect(m_openButton, &QPushButton::clicked, this, &MainWindow::chooseArchive);
    connect(m_browseTargetButton, &QPushButton::clicked, this, &MainWindow::chooseExtractTarget);
    connect(m_extractHereButton, &QPushButton::clicked, this, &MainWindow::extractHere);
    connect(m_extractToButton, &QPushButton::clicked, this, &MainWindow::extractToChosen);
    connect(m_cancelButton, &QPushButton::clicked, this, &MainWindow::cancelCurrentTask);
    connect(m_archiveTree, &QTreeWidget::itemSelectionChanged, this,
            &MainWindow::onArchiveSelectionChanged);
    connect(m_archiveTree, &QTreeWidget::customContextMenuRequested, this,
            &MainWindow::showArchiveContextMenu);
    connect(m_filterEdit, &QLineEdit::textChanged, this, &MainWindow::filterArchiveEntries);
    connect(m_emptyOpenButton, &QPushButton::clicked, this, &MainWindow::chooseArchive);
    connect(m_emptyNewButton, &QPushButton::clicked, this, &MainWindow::startNewArchive);

    applyStyle();
    m_uiReady = true;
    updateArchiveHeader(QString(), 0, 0, false);
    updateActions();
}

void MainWindow::setupActions()
{
    auto icon = [this](QStyle::StandardPixmap pixmap) {
        return style()->standardIcon(pixmap);
    };

    m_openAction = new QAction(icon(QStyle::SP_DialogOpenButton),
                               QStringLiteral("打开压缩包…"), this);
    m_openAction->setShortcut(QKeySequence::Open);
    m_openAction->setToolTip(QStringLiteral("打开本地压缩包 (Ctrl+O)"));
    connect(m_openAction, &QAction::triggered, this, &MainWindow::chooseArchive);

    m_newAction = new QAction(icon(QStyle::SP_FileDialogNewFolder),
                              QStringLiteral("新建压缩任务"), this);
    m_newAction->setShortcut(QKeySequence::New);
    m_newAction->setToolTip(QStringLiteral("切换到压缩任务并添加文件 (Ctrl+N)"));
    connect(m_newAction, &QAction::triggered, this, &MainWindow::startNewArchive);

    m_addFilesAction = new QAction(QStringLiteral("添加文件…"), this);
    connect(m_addFilesAction, &QAction::triggered, this, &MainWindow::addFiles);

    m_addFolderAction = new QAction(QStringLiteral("添加文件夹…"), this);
    connect(m_addFolderAction, &QAction::triggered, this, &MainWindow::addFolder);

    m_compressAction = new QAction(icon(QStyle::SP_DialogSaveButton),
                                   QStringLiteral("开始压缩"), this);
    m_compressAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    connect(m_compressAction, &QAction::triggered, this, &MainWindow::createArchive);

    m_extractHereAction = new QAction(icon(QStyle::SP_ArrowDown),
                                      QStringLiteral("解压到此处"), this);
    m_extractHereAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+E")));
    connect(m_extractHereAction, &QAction::triggered, this, &MainWindow::extractHere);

    m_extractToAction = new QAction(QStringLiteral("解压到…"), this);
    m_extractToAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    connect(m_extractToAction, &QAction::triggered, this, &MainWindow::extractToChosen);

    m_extractSelectedAction = new QAction(QStringLiteral("解压选中项"), this);
    connect(m_extractSelectedAction, &QAction::triggered, this, &MainWindow::extractSelected);

    m_testAction = new QAction(icon(QStyle::SP_DialogApplyButton),
                               QStringLiteral("测试压缩包"), this);
    m_testAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
    connect(m_testAction, &QAction::triggered, this, &MainWindow::testArchive);

    m_cancelAction = new QAction(icon(QStyle::SP_BrowserStop), QStringLiteral("取消任务"), this);
    m_cancelAction->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(m_cancelAction, &QAction::triggered, this, &MainWindow::cancelCurrentTask);

    m_logAction = new QAction(icon(QStyle::SP_FileDialogDetailedView),
                              QStringLiteral("运行日志"), this);
    m_logAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    connect(m_logAction, &QAction::triggered, this, &MainWindow::showLogTab);

    m_aboutAction = new QAction(icon(QStyle::SP_MessageBoxInformation),
                                QStringLiteral("关于 StarZip"), this);
    connect(m_aboutAction, &QAction::triggered, this, &MainWindow::showAbout);

    m_exitAction = new QAction(QStringLiteral("退出"), this);
    m_exitAction->setShortcut(QKeySequence::Quit);
    connect(m_exitAction, &QAction::triggered, this, &QWidget::close);

    m_shellInstallAction = new QAction(QStringLiteral("安装/修复资源管理器右键菜单"), this);
    connect(m_shellInstallAction, &QAction::triggered, this,
            &MainWindow::installShellIntegration);

    m_shellRemoveAction = new QAction(QStringLiteral("移除资源管理器右键菜单"), this);
    connect(m_shellRemoveAction, &QAction::triggered, this,
            &MainWindow::removeShellIntegration);

    addAction(m_openAction);
    addAction(m_newAction);
    addAction(m_extractHereAction);
    addAction(m_extractToAction);
    addAction(m_testAction);
    addAction(m_cancelAction);
    addAction(m_logAction);
    addAction(m_compressAction);
}

void MainWindow::setupMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    fileMenu->addAction(m_openAction);
    m_recentMenu = fileMenu->addMenu(QStringLiteral("最近打开"));
    fileMenu->addSeparator();
    fileMenu->addAction(m_exitAction);

    QMenu *compressMenu = menuBar()->addMenu(QStringLiteral("压缩(&C)"));
    compressMenu->addAction(m_newAction);
    compressMenu->addAction(m_addFilesAction);
    compressMenu->addAction(m_addFolderAction);
    compressMenu->addSeparator();
    compressMenu->addAction(m_compressAction);

    QMenu *actionMenu = menuBar()->addMenu(QStringLiteral("操作(&A)"));
    actionMenu->addAction(m_extractHereAction);
    actionMenu->addAction(m_extractToAction);
    actionMenu->addAction(m_extractSelectedAction);
    actionMenu->addSeparator();
    actionMenu->addAction(m_testAction);
    actionMenu->addAction(m_cancelAction);

    QMenu *viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    viewMenu->addAction(m_logAction);

    QMenu *toolsMenu = menuBar()->addMenu(QStringLiteral("工具(&T)"));
    toolsMenu->addAction(m_shellInstallAction);
    toolsMenu->addAction(m_shellRemoveAction);

    QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    helpMenu->addAction(m_aboutAction);
}

void MainWindow::setupToolBar()
{
    m_toolBar = addToolBar(QStringLiteral("主工具栏"));
    m_toolBar->setObjectName(QStringLiteral("mainToolBar"));
    m_toolBar->setMovable(false);
    m_toolBar->setFloatable(false);
    m_toolBar->setIconSize(QSize(20, 20));
    m_toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto *brand = new QLabel(QStringLiteral("<b>StarZip</b> "
                                            "<span style='color:#6b7280;font-weight:normal'>0.0.3</span>"),
                             m_toolBar);
    brand->setObjectName(QStringLiteral("brandLabel"));
    brand->setContentsMargins(6, 0, 14, 0);
    m_toolBar->addWidget(brand);
    m_toolBar->addAction(m_openAction);
    m_toolBar->addAction(m_newAction);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_extractHereAction);
    m_toolBar->addAction(m_testAction);
    m_toolBar->addSeparator();
    m_toolBar->addAction(m_cancelAction);

    auto *spacer = new QWidget(m_toolBar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolBar->addWidget(spacer);
    m_toolBar->addAction(m_logAction);
}

QWidget *MainWindow::createEmptyPage()
{
    m_emptyPage = new QWidget;
    auto *layout = new QVBoxLayout(m_emptyPage);
    layout->setContentsMargins(40, 30, 40, 30);
    layout->setSpacing(10);
    layout->addStretch(1);

    m_emptyIcon = new QLabel(m_emptyPage);
    m_emptyIcon->setAlignment(Qt::AlignCenter);
    m_emptyIcon->setPixmap(windowIcon().pixmap(72, 72));
    layout->addWidget(m_emptyIcon);

    m_emptyTitle = new QLabel(QStringLiteral("打开或拖入一个压缩包"), m_emptyPage);
    m_emptyTitle->setObjectName(QStringLiteral("emptyTitle"));
    m_emptyTitle->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_emptyTitle);

    m_emptyHint = new QLabel(
        QStringLiteral("支持从本地浏览选择，也可以直接把压缩包拖到窗口。\n"
                       "ZIP / 7Z / RAR 的解压、列表、进度与密码提示保持一致。"),
        m_emptyPage);
    m_emptyHint->setObjectName(QStringLiteral("emptyHint"));
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setWordWrap(true);
    layout->addWidget(m_emptyHint);

    m_emptyFormats = new QLabel(
        QStringLiteral("ZIP · 7Z · RAR · RAR5 · TAR · GZ · XZ · CAB · ISO"), m_emptyPage);
    m_emptyFormats->setObjectName(QStringLiteral("emptyFormats"));
    m_emptyFormats->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_emptyFormats);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->addStretch(1);
    m_emptyOpenButton = new QPushButton(QStringLiteral("打开压缩包…"), m_emptyPage);
    m_emptyOpenButton->setObjectName(QStringLiteral("primaryButton"));
    m_emptyOpenButton->setMinimumWidth(140);
    m_emptyNewButton = new QPushButton(QStringLiteral("新建压缩任务"), m_emptyPage);
    m_emptyNewButton->setMinimumWidth(140);
    buttonRow->addWidget(m_emptyOpenButton);
    buttonRow->addWidget(m_emptyNewButton);
    buttonRow->addStretch(1);
    layout->addSpacing(6);
    layout->addLayout(buttonRow);

    auto *dragHint = new QLabel(
        QStringLiteral("拖入文件或文件夹到“压缩任务”页面，即可直接打包"), m_emptyPage);
    dragHint->setObjectName(QStringLiteral("emptyHint"));
    dragHint->setAlignment(Qt::AlignCenter);
    layout->addWidget(dragHint);
    layout->addStretch(1);
    return m_emptyPage;
}

QWidget *MainWindow::createArchivePage()
{
    m_archivePage = new QWidget;
    auto *pageLayout = new QVBoxLayout(m_archivePage);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);

    m_archiveStack = new QStackedWidget(m_archivePage);
    pageLayout->addWidget(m_archiveStack);

    m_archiveStack->addWidget(createEmptyPage());

    m_contentPage = new QWidget(m_archivePage);
    auto *contentLayout = new QVBoxLayout(m_contentPage);
    contentLayout->setContentsMargins(8, 8, 8, 8);
    contentLayout->setSpacing(8);

    auto *headerCard = new QFrame(m_contentPage);
    headerCard->setObjectName(QStringLiteral("infoCard"));
    auto *headerLayout = new QHBoxLayout(headerCard);
    headerLayout->setContentsMargins(10, 8, 10, 8);
    headerLayout->setSpacing(10);
    m_formatBadge = new QLabel(QStringLiteral("—"), headerCard);
    m_formatBadge->setObjectName(QStringLiteral("formatBadge"));
    m_formatBadge->setAlignment(Qt::AlignCenter);
    m_formatBadge->setMinimumWidth(72);
    m_archiveInfoLabel = new QLabel(QStringLiteral("尚未打开压缩包。"), headerCard);
    m_archiveInfoLabel->setObjectName(QStringLiteral("archiveInfoLabel"));
    m_archiveInfoLabel->setWordWrap(true);
    m_archiveInfoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_archiveInfoLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *filterCaption = new QLabel(QStringLiteral("筛选"), headerCard);
    filterCaption->setObjectName(QStringLiteral("fieldLabel"));
    m_filterEdit = new QLineEdit(headerCard);
    m_filterEdit->setPlaceholderText(QStringLiteral("按名称筛选…"));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setFixedWidth(210);
    headerLayout->addWidget(m_formatBadge);
    headerLayout->addWidget(m_archiveInfoLabel, 1);
    headerLayout->addWidget(filterCaption);
    headerLayout->addWidget(m_filterEdit);
    contentLayout->addWidget(headerCard);

    m_archiveTree = new QTreeWidget(m_contentPage);
    m_archiveTree->setColumnCount(5);
    m_archiveTree->setHeaderLabels({QStringLiteral("名称"), QStringLiteral("大小"),
                                    QStringLiteral("类型"), QStringLiteral("修改时间"),
                                    QStringLiteral("加密")});
    m_archiveTree->setRootIsDecorated(true);
    m_archiveTree->setAlternatingRowColors(true);
    m_archiveTree->setUniformRowHeights(true);
    m_archiveTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_archiveTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_archiveTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_archiveTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_archiveTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_archiveTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_archiveTree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    contentLayout->addWidget(m_archiveTree, 1);

    auto *bottomRow = new QHBoxLayout();
    bottomRow->setSpacing(8);
    m_extractSelectedButton = new QPushButton(QStringLiteral("解压选中项…"), m_contentPage);
    m_extractSelectedButton->setToolTip(QStringLiteral("仅解压列表中选中的文件或文件夹"));
    m_testButton = new QPushButton(QStringLiteral("测试压缩包"), m_contentPage);
    m_testButton->setToolTip(QStringLiteral("读取并校验全部数据（Ctrl+T）"));
    auto *treeHint = new QLabel(
        QStringLiteral("双击文件夹展开/折叠 · 右键可复制路径或解压选中项"), m_contentPage);
    treeHint->setObjectName(QStringLiteral("mutedLabel"));
    bottomRow->addWidget(m_extractSelectedButton);
    bottomRow->addWidget(m_testButton);
    bottomRow->addStretch(1);
    bottomRow->addWidget(treeHint);
    contentLayout->addLayout(bottomRow);

    connect(m_extractSelectedButton, &QPushButton::clicked, this, &MainWindow::extractSelected);
    connect(m_testButton, &QPushButton::clicked, this, &MainWindow::testArchive);

    m_archiveStack->addWidget(m_contentPage);
    m_archiveStack->setCurrentWidget(m_emptyPage);
    return m_archivePage;
}

QWidget *MainWindow::createCompressPage()
{
    m_compressPage = new QWidget;
    auto *layout = new QVBoxLayout(m_compressPage);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *sourceRow = new QHBoxLayout();
    sourceRow->setSpacing(8);
    auto *sourceCaption = new QLabel(QStringLiteral("待压缩内容"), m_compressPage);
    sourceCaption->setObjectName(QStringLiteral("sectionLabel"));
    m_addFilesButton = new QPushButton(QStringLiteral("添加文件…"), m_compressPage);
    m_addFolderButton = new QPushButton(QStringLiteral("添加文件夹…"), m_compressPage);
    m_removeSourceButton = new QPushButton(QStringLiteral("移除选中"), m_compressPage);
    m_clearSourcesButton = new QPushButton(QStringLiteral("清空"), m_compressPage);
    sourceRow->addWidget(sourceCaption);
    sourceRow->addWidget(m_addFilesButton);
    sourceRow->addWidget(m_addFolderButton);
    sourceRow->addWidget(m_removeSourceButton);
    sourceRow->addWidget(m_clearSourcesButton);
    sourceRow->addStretch(1);
    layout->addLayout(sourceRow);

    m_sourcesTree = new QTreeWidget(m_compressPage);
    m_sourcesTree->setColumnCount(4);
    m_sourcesTree->setHeaderLabels({QStringLiteral("名称"), QStringLiteral("类型"),
                                    QStringLiteral("大小"), QStringLiteral("路径")});
    m_sourcesTree->setRootIsDecorated(false);
    m_sourcesTree->setAlternatingRowColors(true);
    m_sourcesTree->setUniformRowHeights(true);
    m_sourcesTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_sourcesTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_sourcesTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_sourcesTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_sourcesTree->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    layout->addWidget(m_sourcesTree, 1);

    auto *settingsBox = new QGroupBox(QStringLiteral("压缩设置"), m_compressPage);
    auto *grid = new QGridLayout(settingsBox);
    grid->setContentsMargins(12, 14, 12, 10);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);

    auto *formatLabel = new QLabel(QStringLiteral("输出格式"), settingsBox);
    formatLabel->setObjectName(QStringLiteral("fieldLabel"));
    m_formatCombo = new QComboBox(settingsBox);
    m_levelCombo = new QComboBox(settingsBox);
    const QVector<FormatChoice> choices = formatChoices();
    for (const FormatChoice &choice : choices)
        m_formatCombo->addItem(choice.label, choice.id);
    m_levelCombo->addItem(QStringLiteral("仅存储 (0)"), 0);
    m_levelCombo->addItem(QStringLiteral("快速 (1)"), 1);
    m_levelCombo->addItem(QStringLiteral("标准 (6)"), 6);
    m_levelCombo->addItem(QStringLiteral("较高 (7)"), 7);
    m_levelCombo->addItem(QStringLiteral("极限 (9)"), 9);
    m_levelCombo->setCurrentIndex(2);

    auto *levelLabel = new QLabel(QStringLiteral("压缩级别"), settingsBox);
    levelLabel->setObjectName(QStringLiteral("fieldLabel"));
    grid->addWidget(formatLabel, 0, 0);
    grid->addWidget(m_formatCombo, 0, 1);
    grid->addWidget(levelLabel, 0, 2);
    grid->addWidget(m_levelCombo, 0, 3);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);

    auto *passwordLabel = new QLabel(QStringLiteral("密码"), settingsBox);
    passwordLabel->setObjectName(QStringLiteral("fieldLabel"));
    m_passwordEdit = new QLineEdit(settingsBox);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(QStringLiteral("可选：为 ZIP 设置解压密码"));
    m_showPasswordCheck = new QCheckBox(QStringLiteral("显示"), settingsBox);
    m_passwordNote = new QLabel(settingsBox);
    m_passwordNote->setObjectName(QStringLiteral("mutedLabel"));
    m_passwordNote->setWordWrap(true);
    grid->addWidget(passwordLabel, 1, 0);
    grid->addWidget(m_passwordEdit, 1, 1);
    grid->addWidget(m_showPasswordCheck, 1, 2);
    grid->addWidget(m_passwordNote, 1, 3, 1, 2);

    auto *outputLabel = new QLabel(QStringLiteral("输出文件"), settingsBox);
    outputLabel->setObjectName(QStringLiteral("fieldLabel"));
    m_outputEdit = new QLineEdit(settingsBox);
    m_outputEdit->setPlaceholderText(QStringLiteral("留空会在压缩时询问保存位置"));
    m_browseOutputButton = new QPushButton(QStringLiteral("浏览…"), settingsBox);
    grid->addWidget(outputLabel, 2, 0);
    grid->addWidget(m_outputEdit, 2, 1, 1, 2);
    grid->addWidget(m_browseOutputButton, 2, 3);

    auto *compressHint = new QLabel(
        QStringLiteral("ZIP 支持 AES-256 密码；7Z 使用 LZMA2；TAR 系列适合跨平台归档。"
                       "创建 RAR 不在本版本范围内。"),
        settingsBox);
    compressHint->setObjectName(QStringLiteral("mutedLabel"));
    compressHint->setWordWrap(true);
    m_createArchiveButton = new QPushButton(QStringLiteral("开始压缩"), settingsBox);
    m_createArchiveButton->setObjectName(QStringLiteral("primaryButton"));
    m_createArchiveButton->setMinimumWidth(130);
    grid->addWidget(compressHint, 3, 0, 1, 3);
    grid->addWidget(m_createArchiveButton, 3, 3, Qt::AlignRight);
    layout->addWidget(settingsBox);

    connect(m_addFilesButton, &QPushButton::clicked, this, &MainWindow::addFiles);
    connect(m_addFolderButton, &QPushButton::clicked, this, &MainWindow::addFolder);
    connect(m_removeSourceButton, &QPushButton::clicked, this,
            &MainWindow::removeSelectedSources);
    connect(m_clearSourcesButton, &QPushButton::clicked, this, &MainWindow::clearSources);
    connect(m_sourcesTree, &QTreeWidget::itemSelectionChanged, this, &MainWindow::updateActions);
    connect(m_formatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onFormatChanged);
    connect(m_showPasswordCheck, &QCheckBox::toggled, this, &MainWindow::togglePasswordVisible);
    connect(m_browseOutputButton, &QPushButton::clicked, this, &MainWindow::browseOutput);
    connect(m_createArchiveButton, &QPushButton::clicked, this, &MainWindow::createArchive);

    onFormatChanged();
    return m_compressPage;
}

QWidget *MainWindow::createLogPage()
{
    m_logPage = new QWidget;
    auto *layout = new QVBoxLayout(m_logPage);
    layout->setContentsMargins(8, 8, 8, 8);
    m_logView = new QPlainTextEdit(m_logPage);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(5000);
    m_logView->setPlaceholderText(QStringLiteral("任务日志将显示在这里。"));
    layout->addWidget(m_logView);
    return m_logPage;
}

void MainWindow::applyStyle()
{
    setStyleSheet(QStringLiteral(R"(
        QWidget#appRoot { background: #eef1f5; }
        QMainWindow { background: #eef1f5; }
        QMenuBar { background: #ffffff; border-bottom: 1px solid #d7dce4; padding: 1px 4px; }
        QMenuBar::item { padding: 5px 10px; background: transparent; }
        QMenuBar::item:selected { background: #eaf0fa; border-radius: 3px; }
        QMenu { background: #ffffff; border: 1px solid #cbd2dc; padding: 4px; }
        QMenu::item { padding: 6px 28px 6px 22px; }
        QMenu::item:selected { background: #e8effb; color: #17346b; }
        QMenu::separator { height: 1px; background: #e4e8ee; margin: 4px 8px; }
        QToolBar#mainToolBar {
            background: #ffffff; border-bottom: 1px solid #d7dce4;
            spacing: 3px; padding: 4px 8px;
        }
        QToolBar#mainToolBar QToolButton {
            padding: 4px 9px; border: 1px solid transparent; border-radius: 3px;
            color: #263449;
        }
        QToolBar#mainToolBar QToolButton:hover {
            background: #eef3fb; border-color: #c9d5e6;
        }
        QToolBar#mainToolBar QToolButton:pressed { background: #dfe9f8; }
        QToolBar#mainToolBar QToolButton:disabled { color: #98a1b1; }
        QLabel#brandLabel { color: #1f2f4d; }
        QFrame#pathCard, QFrame#infoCard, QFrame#progressCard {
            background: #ffffff; border: 1px solid #d5dbe4; border-radius: 4px;
        }
        QLabel#fieldLabel, QLabel#sectionLabel { color: #5b6577; }
        QLabel#sectionLabel { font-weight: 600; }
        QLabel#archivePathLabel { color: #1d2b44; }
        QLabel#mutedLabel { color: #7a8496; }
        QLabel#formatBadge {
            background: #e8f0ff; color: #1d4ed8; border: 1px solid #bfd3ff;
            border-radius: 4px; font-weight: 600; padding: 4px 8px;
        }
        QLabel#emptyTitle { font-size: 19pt; font-weight: 600; color: #1f2a3d; }
        QLabel#emptyHint { color: #6f7a8c; }
        QLabel#emptyFormats {
            color: #2f5fbf; background: #f2f6fd; border: 1px solid #d9e5fa;
            border-radius: 11px; padding: 3px 14px;
        }
        QPushButton {
            background: #ffffff; color: #263449; border: 1px solid #c8d0dc;
            border-radius: 3px; padding: 5px 12px; min-height: 22px;
        }
        QPushButton:hover { background: #f4f7fb; border-color: #aebbd0; }
        QPushButton:pressed { background: #e8eef7; }
        QPushButton:disabled { color: #9aa3b3; background: #f4f5f8; border-color: #e1e5ec; }
        QPushButton#primaryButton {
            background: #2563eb; color: #ffffff; border-color: #2563eb; font-weight: 600;
        }
        QPushButton#primaryButton:hover { background: #1d4ed8; border-color: #1d4ed8; }
        QPushButton#primaryButton:pressed { background: #1e40af; }
        QPushButton#primaryButton:disabled {
            background: #b9c8e8; border-color: #b9c8e8; color: #f1f5ff;
        }
        QLineEdit, QComboBox {
            background: #ffffff; color: #1f2a3d; border: 1px solid #c8d0dc;
            border-radius: 3px; padding: 4px 7px; min-height: 22px;
        }
        QLineEdit:focus, QComboBox:focus { border-color: #2563eb; }
        QLineEdit:disabled, QComboBox:disabled { background: #f4f5f8; color: #9aa3b3; }
        QComboBox::drop-down { border: none; width: 20px; }
        QCheckBox { color: #3d4759; spacing: 5px; }
        QGroupBox {
            background: #ffffff; border: 1px solid #d5dbe4; border-radius: 4px;
            margin-top: 9px; padding-top: 8px; font-weight: 600; color: #334155;
        }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }
        QTabWidget::pane { border: 1px solid #d5dbe4; background: #ffffff; }
        QTabBar::tab {
            background: #e4e9f0; color: #4b5567; border: 1px solid #d5dbe4;
            border-bottom: none; padding: 6px 18px; margin-right: 2px;
        }
        QTabBar::tab:selected { background: #ffffff; color: #17408f; font-weight: 600; }
        QTabBar::tab:hover:!selected { background: #edf1f7; }
        QTreeWidget, QPlainTextEdit {
            background: #ffffff; border: 1px solid #d5dbe4; alternate-background-color: #fafbfd;
            selection-background-color: #dbeafe; selection-color: #172033;
        }
        QTreeWidget::item { height: 24px; }
        QTreeWidget::item:hover { background: #edf4ff; }
        QHeaderView::section {
            background: #f3f5f8; color: #4b5567; border: none;
            border-right: 1px solid #e2e6ed; border-bottom: 1px solid #d5dbe4;
            padding: 5px 6px;
        }
        QProgressBar {
            border: 1px solid #d5dbe4; border-radius: 3px; background: #ffffff;
            text-align: center; min-height: 20px; color: #334155;
        }
        QProgressBar::chunk { background: #2563eb; }
        QStatusBar { background: #e8ecf2; color: #4b5567; border-top: 1px solid #d5dbe4; }
        QStatusBar QLabel { color: #5b6577; padding: 0 6px; }
        QSplitter::handle { background: #eef1f5; }
    )"));
}

void MainWindow::rebuildRecentMenu()
{
    if (!m_recentMenu)
        return;
    m_recentMenu->clear();
    QSettings settings;
    const QStringList recent = settings.value(QStringLiteral("recentFiles")).toStringList();
    if (recent.isEmpty()) {
        QAction *empty = m_recentMenu->addAction(QStringLiteral("暂无记录"));
        empty->setEnabled(false);
        return;
    }
    for (const QString &path : recent) {
        if (!QFileInfo::exists(path))
            continue;
        QAction *action = m_recentMenu->addAction(archiveDisplayName(path));
        action->setToolTip(QDir::toNativeSeparators(path));
        connect(action, &QAction::triggered, this, [this, path]() { openArchive(path); });
    }
    m_recentMenu->addSeparator();
    QAction *clear = m_recentMenu->addAction(QStringLiteral("清除记录"));
    connect(clear, &QAction::triggered, this, &MainWindow::clearRecentFiles);
}

void MainWindow::addRecentFile(const QString &path)
{
    if (path.isEmpty())
        return;
    QSettings settings;
    QStringList recent = settings.value(QStringLiteral("recentFiles")).toStringList();
    for (int i = recent.size() - 1; i >= 0; --i) {
        if (recent.at(i).compare(path, Qt::CaseInsensitive) == 0)
            recent.removeAt(i);
    }
    recent.prepend(path);
    while (recent.size() > 8)
        recent.removeLast();
    settings.setValue(QStringLiteral("recentFiles"), recent);
    rebuildRecentMenu();
}

void MainWindow::clearRecentFiles()
{
    QSettings settings;
    settings.remove(QStringLiteral("recentFiles"));
    rebuildRecentMenu();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_busy || !event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }
    for (const QUrl &url : event->mimeData()->urls()) {
        if (urlIsLocalPath(url)) {
            event->acceptProposedAction();
            statusBar()->showMessage(QStringLiteral("松开鼠标：打开压缩包，或添加到压缩任务。"));
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
        if (urlIsLocalPath(url)) {
            event->acceptProposedAction();
            return;
        }
    }
    event->ignore();
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent *event)
{
    statusBar()->showMessage(QStringLiteral("就绪"), 2000);
    QMainWindow::dragLeaveEvent(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (m_busy) {
        event->ignore();
        return;
    }

    QStringList localPaths;
    if (event->mimeData()->hasUrls()) {
        for (const QUrl &url : event->mimeData()->urls()) {
            if (urlIsLocalPath(url))
                localPaths.append(url.toLocalFile());
        }
    }
    if (localPaths.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();

    const bool compressDrop = m_tabs->currentWidget() == m_compressPage;
    if (compressDrop || localPaths.size() > 1 || QFileInfo(localPaths.first()).isDir()) {
        for (const QString &path : localPaths)
            addSourcePath(path);
        refreshSourcesSummary();
        m_tabs->setCurrentWidget(m_compressPage);
        statusBar()->showMessage(QStringLiteral("已添加 %1 个待压缩项目。").arg(localPaths.size()),
                                 5000);
        appendLog(QStringLiteral("拖入并添加 %1 个待压缩项目。").arg(localPaths.size()));
        return;
    }

    appendLog(QStringLiteral("拖入压缩包：%1")
                  .arg(QDir::toNativeSeparators(localPaths.first())));
    openArchive(localPaths.first());
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

    QSettings settings;
    settings.setValue(QStringLiteral("windowGeometry"), saveGeometry());
    event->accept();
}

void MainWindow::openArchivePath(const QString &path)
{
    if (path.isEmpty())
        return;
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        m_startupAction = StartupAction::None;
        showError(QStringLiteral("无法打开压缩包"),
                  QStringLiteral("文件不存在或不是普通文件：\n%1")
                      .arg(QDir::toNativeSeparators(path)));
        return;
    }
    m_tabs->setCurrentWidget(m_archivePage);
    openArchive(info.absoluteFilePath());
}

void MainWindow::applyStartupArguments(const QStringList &arguments)
{
    m_startupAction = StartupAction::None;

    for (int i = 1; i < arguments.size(); ++i) {
        const QString arg = arguments.at(i);

        if (arg == QStringLiteral("--compress")) {
            bool added = false;
            while (i + 1 < arguments.size()
                   && !arguments.at(i + 1).startsWith(QStringLiteral("--"))) {
                const QFileInfo info(arguments.at(++i));
                if (!info.exists()) {
                    appendLog(QStringLiteral("忽略不存在的路径：%1")
                                  .arg(QDir::toNativeSeparators(info.absoluteFilePath())));
                    continue;
                }
                addSourcePath(info.absoluteFilePath());
                added = true;
            }
            if (added) {
                m_tabs->setCurrentWidget(m_compressPage);
                refreshSourcesSummary();
            }
            continue;
        }

        if (arg == QStringLiteral("--extract-here") || arg == QStringLiteral("--extract-to")
            || arg == QStringLiteral("--test")) {
            if (i + 1 >= arguments.size())
                continue;
            const QString path = arguments.at(++i);
            m_startupAction = arg == QStringLiteral("--extract-here")
                                  ? StartupAction::ExtractHere
                                  : (arg == QStringLiteral("--extract-to")
                                         ? StartupAction::ExtractTo
                                         : StartupAction::Test);
            openArchivePath(path);
            continue;
        }

        if (arg.startsWith(QLatin1Char('-')))
            continue;

        const QFileInfo info(arg);
        if (info.exists() && info.isFile())
            openArchivePath(info.absoluteFilePath());
    }
}

void MainWindow::runStartupAction()
{
    if (m_startupAction == StartupAction::None || m_archivePath.isEmpty())
        return;

    const StartupAction action = m_startupAction;
    m_startupAction = StartupAction::None;

    if (action == StartupAction::ExtractHere) {
        const QFileInfo archiveInfo(m_archivePath);
        QString base = archiveInfo.completeBaseName();
        if (base.isEmpty())
            base = QStringLiteral("StarZip_extracted");
        const QString destination =
            QDir(archiveInfo.absolutePath()).filePath(base);
        m_extractTarget = QDir(destination).absolutePath();
        m_targetEdit->setText(QDir::toNativeSeparators(m_extractTarget));
        startExtraction(m_extractTarget, QStringList(), m_archivePassword);
    } else if (action == StartupAction::ExtractTo) {
        extractToChosen();
    } else if (action == StartupAction::Test) {
        startTest(m_archivePassword);
    }
}

void MainWindow::chooseArchive()
{
    if (m_busy) {
        statusBar()->showMessage(QStringLiteral("当前有任务正在进行，请稍候。"), 4000);
        return;
    }
    QSettings settings;
    const QString start = settings.value(QStringLiteral("lastOpenDir"), QDir::currentPath())
                              .toString();
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开压缩包"), start,
        QStringLiteral("压缩包 (*.zip *.7z *.rar *.tar *.gz *.tgz *.bz2 *.xz *.lzma *.cab *.iso);;"
                       "所有文件 (*.*)"));
    if (path.isEmpty())
        return;
    settings.setValue(QStringLiteral("lastOpenDir"), QFileInfo(path).absolutePath());
    openArchive(path);
}

void MainWindow::openArchive(const QString &path, const QString &password, bool retry)
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

    if (!retry) {
        m_archivePassword.clear();
        m_passwordAttempts = 0;
        m_passwordSignal = false;
    }
    if (!password.isEmpty())
        m_archivePassword = password;

    m_archivePath = info.absoluteFilePath();
    m_archiveLabel->setText(QDir::toNativeSeparators(m_archivePath));
    m_archiveLabel->setToolTip(QDir::toNativeSeparators(m_archivePath));
    m_archiveInfoLabel->setText(QStringLiteral("正在读取压缩包…"));
    m_formatBadge->setText(QStringLiteral("…"));
    m_archiveTree->clear();
    m_entries.clear();
    m_formatName.clear();
    m_filterEdit->clear();
    m_emptyTitle->setText(QStringLiteral("正在读取压缩包…"));
    m_emptyHint->setText(QDir::toNativeSeparators(m_archivePath));
    m_archiveStack->setCurrentWidget(m_emptyPage);
    m_tabs->setCurrentWidget(m_archivePage);
    m_statusFormatLabel->setText(QStringLiteral("格式：读取中"));
    m_statusCountLabel->setText(QStringLiteral("条目：—"));
    setWindowTitle(QStringLiteral("StarZip 0.0.3 — %1").arg(archiveDisplayName(m_archivePath)));
    appendLog(QStringLiteral("打开压缩包：%1").arg(QDir::toNativeSeparators(m_archivePath)));

    const QString archivePath = m_archivePath;
    if (!beginJob(ArchiveWorker::List, QStringLiteral("正在识别压缩包…"), true,
                  [archivePath, password](ArchiveWorker *worker) {
                      worker->listArchive(archivePath, password);
                  })) {
        resetArchiveState();
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
                                 QStringLiteral("请先打开压缩包，或把压缩包拖到窗口中。"));
        return;
    }
    QString destination = m_targetEdit->text().trimmed();
    if (destination.isEmpty())
        destination = QDir::currentPath();
    startExtraction(destination, QStringList(), m_archivePassword);
}

void MainWindow::extractToChosen()
{
    if (m_archivePath.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("尚未打开压缩包"),
                                 QStringLiteral("请先打开压缩包，或把压缩包拖到窗口中。"));
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
    startExtraction(m_extractTarget, QStringList(), m_archivePassword);
}

void MainWindow::extractSelected()
{
    if (m_archivePath.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("尚未打开压缩包"),
                                 QStringLiteral("请先打开压缩包。"));
        return;
    }
    const QStringList paths = selectedArchivePaths();
    if (paths.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("未选择条目"),
                                 QStringLiteral("请先在列表中选择要解压的文件或文件夹。"));
        return;
    }
    QString destination = m_targetEdit->text().trimmed();
    if (destination.isEmpty())
        destination = QDir::currentPath();
    startExtraction(destination, paths, m_archivePassword);
}

void MainWindow::testArchive()
{
    if (m_archivePath.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("尚未打开压缩包"),
                                 QStringLiteral("请先打开压缩包。"));
        return;
    }
    startTest(m_archivePassword);
}

void MainWindow::startExtraction(const QString &destination, const QStringList &onlyPaths,
                                 const QString &password, bool retry)
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

    if (!retry) {
        m_passwordAttempts = 0;
        m_passwordSignal = false;
    }

    QString effectivePassword = password;
    if (effectivePassword.isEmpty() && archiveHasEncryptedEntries()) {
        effectivePassword = promptPassword(
            QStringLiteral("加密压缩包"),
            QStringLiteral("该压缩包包含加密条目，请输入密码（留空则取消）："));
        if (effectivePassword.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("已取消：需要密码才能解压。"), 6000);
            return;
        }
    }
    if (!effectivePassword.isEmpty())
        m_archivePassword = effectivePassword;

    m_extractTarget = QDir(target).absolutePath();
    m_targetEdit->setText(QDir::toNativeSeparators(m_extractTarget));
    m_lastExtractDest = m_extractTarget;
    m_lastExtractOnly = onlyPaths;

    const QString archivePath = m_archivePath;
    const QString destinationDir = m_extractTarget;
    appendLog(QStringLiteral("开始解压：%1 → %2%3")
                  .arg(QDir::toNativeSeparators(archivePath),
                       QDir::toNativeSeparators(destinationDir),
                       onlyPaths.isEmpty()
                           ? QString()
                           : QStringLiteral("（选中 %1 项）").arg(onlyPaths.size())));

    beginJob(ArchiveWorker::Extract, QStringLiteral("正在解压…"), false,
             [archivePath, destinationDir, effectivePassword, onlyPaths](ArchiveWorker *worker) {
                 worker->extractArchive(archivePath, destinationDir, effectivePassword, onlyPaths);
             });
}

void MainWindow::startTest(const QString &password, bool retry)
{
    if (m_archivePath.isEmpty() || m_busy)
        return;
    if (!retry) {
        m_passwordAttempts = 0;
        m_passwordSignal = false;
    }

    QString effectivePassword = password;
    if (effectivePassword.isEmpty() && archiveHasEncryptedEntries()) {
        effectivePassword = promptPassword(
            QStringLiteral("加密压缩包"),
            QStringLiteral("测试加密压缩包需要密码，请输入："));
        if (effectivePassword.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("已取消：需要密码才能测试。"), 6000);
            return;
        }
    }
    if (!effectivePassword.isEmpty())
        m_archivePassword = effectivePassword;

    const QString archivePath = m_archivePath;
    appendLog(QStringLiteral("开始测试压缩包：%1")
                  .arg(QDir::toNativeSeparators(archivePath)));
    beginJob(ArchiveWorker::Test, QStringLiteral("正在校验压缩包…"), false,
             [archivePath, effectivePassword](ArchiveWorker *worker) {
                 worker->testArchive(archivePath, effectivePassword);
             });
}

void MainWindow::addFiles()
{
    if (m_busy)
        return;
    QString startDir = QDir::homePath();
    if (m_sourcesTree->topLevelItemCount() > 0) {
        startDir = QFileInfo(m_sourcesTree->topLevelItem(0)
                                 ->data(0, Qt::UserRole)
                                 .toString())
                       .absolutePath();
    }
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择要压缩的文件"), startDir, QStringLiteral("所有文件 (*.*)"));
    for (const QString &file : files)
        addSourcePath(file);
    refreshSourcesSummary();
    if (!files.isEmpty())
        m_tabs->setCurrentWidget(m_compressPage);
}

void MainWindow::addFolder()
{
    if (m_busy)
        return;
    QString startDir = QDir::homePath();
    if (m_sourcesTree->topLevelItemCount() > 0) {
        startDir = QFileInfo(m_sourcesTree->topLevelItem(0)
                                 ->data(0, Qt::UserRole)
                                 .toString())
                       .absolutePath();
    }
    const QString folder = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择要压缩的文件夹"), startDir);
    if (folder.isEmpty())
        return;
    addSourcePath(folder);
    refreshSourcesSummary();
    m_tabs->setCurrentWidget(m_compressPage);
}

void MainWindow::addSourcePath(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        showError(QStringLiteral("无法添加"),
                  QStringLiteral("路径不存在：\n%1").arg(QDir::toNativeSeparators(path)));
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
    item->setText(2, info.isDir() ? QStringLiteral("—") : humanSize(info.size()));
    item->setText(3, QDir::toNativeSeparators(absolute));
    item->setData(0, Qt::UserRole, absolute);
    item->setToolTip(3, QDir::toNativeSeparators(absolute));
    item->setIcon(0, style()->standardIcon(info.isDir() ? QStyle::SP_DirIcon
                                                        : QStyle::SP_FileIcon));
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

QString MainWindow::currentFormat() const
{
    return m_formatCombo ? m_formatCombo->currentData().toString() : QStringLiteral("zip");
}

MainWindow::FormatChoice MainWindow::currentFormatChoice() const
{
    const QString id = currentFormat();
    for (const FormatChoice &choice : formatChoices()) {
        if (choice.id == id)
            return choice;
    }
    return formatChoices().first();
}

QVector<MainWindow::FormatChoice> MainWindow::formatChoices()
{
    return {
        {QStringLiteral("zip"), QStringLiteral("ZIP（.zip）"), QStringLiteral(".zip"),
         QStringLiteral("ZIP 压缩包 (*.zip)"), true, true},
        {QStringLiteral("7z"), QStringLiteral("7Z（.7z）"), QStringLiteral(".7z"),
         QStringLiteral("7Z 压缩包 (*.7z)"), ExternalEngine::available(), true},
        {QStringLiteral("tar"), QStringLiteral("TAR（.tar）"), QStringLiteral(".tar"),
         QStringLiteral("TAR 归档 (*.tar)"), false, false},
        {QStringLiteral("tar.gz"), QStringLiteral("TAR.GZ（.tar.gz）"), QStringLiteral(".tar.gz"),
         QStringLiteral("TAR.GZ 压缩包 (*.tar.gz)"), false, true},
        {QStringLiteral("tar.xz"), QStringLiteral("TAR.XZ（.tar.xz）"), QStringLiteral(".tar.xz"),
         QStringLiteral("TAR.XZ 压缩包 (*.tar.xz)"), false, true},
    };
}

QString MainWindow::ensureSuffix(const QString &path, const QString &suffix)
{
    if (path.endsWith(suffix, Qt::CaseInsensitive))
        return path;
    return path + suffix;
}

QString MainWindow::replaceSuffix(const QString &path, const QString &suffix)
{
    if (path.isEmpty())
        return path;
    static const QStringList known = {QStringLiteral(".tar.gz"), QStringLiteral(".tar.xz"),
                                      QStringLiteral(".zip"), QStringLiteral(".7z"),
                                      QStringLiteral(".tar")};
    QString base = path;
    for (const QString &ending : known) {
        if (base.endsWith(ending, Qt::CaseInsensitive)) {
            base.chop(ending.size());
            break;
        }
    }
    return base + suffix;
}

void MainWindow::onFormatChanged()
{
    if (!m_formatCombo || !m_passwordEdit)
        return;
    const FormatChoice choice = currentFormatChoice();
    m_passwordEdit->setEnabled(choice.supportsPassword && !m_busy);
    m_showPasswordCheck->setEnabled(choice.supportsPassword && !m_busy);
    m_levelCombo->setEnabled(choice.supportsLevel && !m_busy);

    if (choice.id == QStringLiteral("zip")) {
        m_passwordNote->setText(QStringLiteral("ZIP 支持 AES-256 密码；如果运行库不支持，"
                                               "会自动回退到 ZipCrypto。"));
    } else if (choice.id == QStringLiteral("7z")) {
        m_passwordNote->setText(
            ExternalEngine::available()
                ? QStringLiteral("7Z 密码由 7-Zip 引擎写入（AES-256）；留空则不加密。")
                : QStringLiteral("未找到 7-Zip 引擎，7Z 暂不能设置密码；请选择 ZIP。"));
    } else {
        m_passwordNote->setText(QStringLiteral("TAR 系列不支持密码，适合跨平台归档。"));
    }

    if (!m_outputEdit->text().isEmpty()) {
        const QString updated = replaceSuffix(m_outputEdit->text(), choice.suffix);
        if (updated != m_outputEdit->text())
            m_outputEdit->setText(updated);
    }
    updateSuggestedOutput();
    refreshSourcesSummary();
}

void MainWindow::togglePasswordVisible(bool visible)
{
    m_passwordEdit->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
}

void MainWindow::browseOutput()
{
    const FormatChoice choice = currentFormatChoice();
    QString initial = m_outputEdit->text().trimmed();
    if (initial.isEmpty()) {
        const QStringList sources = selectedSources();
        QString base = QStringLiteral("StarZip_%1")
                           .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
        QString dir = QDir::homePath();
        if (!sources.isEmpty()) {
            const QFileInfo first(sources.first());
            dir = first.isDir() ? first.absolutePath() : first.absolutePath();
            if (sources.size() == 1) {
                const QString name = first.isDir() ? first.fileName() : first.completeBaseName();
                if (!name.isEmpty())
                    base = name;
            }
        }
        initial = QDir(dir).filePath(base + choice.suffix);
    }
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存压缩包"), initial,
                                                      choice.dialogFilter);
    if (path.isEmpty())
        return;
    m_outputEdit->setText(ensureSuffix(path, choice.suffix));
    m_outputEdit->setToolTip(QDir::toNativeSeparators(m_outputEdit->text()));
}

void MainWindow::createArchive()
{
    if (m_busy) {
        statusBar()->showMessage(QStringLiteral("当前有任务正在进行，请稍候。"), 4000);
        return;
    }
    const QStringList sources = selectedSources();
    if (sources.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("没有待压缩内容"),
                                 QStringLiteral("请先添加要压缩的文件或文件夹。"));
        m_tabs->setCurrentWidget(m_compressPage);
        return;
    }

    const FormatChoice choice = currentFormatChoice();
    const QString password = choice.supportsPassword ? m_passwordEdit->text() : QString();

    QString output = m_outputEdit->text().trimmed();
    if (output.isEmpty()) {
        browseOutput();
        output = m_outputEdit->text().trimmed();
        if (output.isEmpty())
            return;
    }
    output = ensureSuffix(output, choice.suffix);
    m_outputEdit->setText(output);

    if (QFileInfo::exists(output)) {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, QStringLiteral("覆盖文件"),
            QStringLiteral("输出文件已存在，是否覆盖？\n%1")
                .arg(QDir::toNativeSeparators(output)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            statusBar()->showMessage(QStringLiteral("已取消压缩。"), 4000);
            return;
        }
    }

    CompressOptions options;
    options.format = choice.id;
    options.level = m_levelCombo->currentData().toInt();
    options.password = password;

    appendLog(QStringLiteral("开始压缩 %1 个源项目 → %2（格式 %3%4）")
                  .arg(sources.size())
                  .arg(QDir::toNativeSeparators(output), choice.label,
                       password.isEmpty() ? QString()
                                          : QStringLiteral("，已启用密码")));
    beginJob(ArchiveWorker::Compress, QStringLiteral("正在准备压缩…"), false,
             [sources, output, options](ArchiveWorker *worker) {
                 worker->compressArchive(sources, output, options);
             });
}

void MainWindow::startNewArchive()
{
    if (m_busy)
        return;
    m_tabs->setCurrentWidget(m_compressPage);
    if (m_sourcesTree->topLevelItemCount() == 0)
        addFiles();
}

void MainWindow::refreshSourcesSummary()
{
    const int count = m_sourcesTree->topLevelItemCount();
    if (m_createArchiveButton) {
        m_createArchiveButton->setText(count > 0
                                          ? QStringLiteral("开始压缩（%1 项）").arg(count)
                                          : QStringLiteral("开始压缩"));
    }
    updateSuggestedOutput();
    updateActions();
}

void MainWindow::updateSuggestedOutput()
{
    if (!m_outputEdit)
        return;
    if (!m_outputEdit->text().trimmed().isEmpty())
        return;
    const FormatChoice choice = currentFormatChoice();
    const QStringList sources = selectedSources();
    if (sources.isEmpty()) {
        m_outputEdit->setPlaceholderText(QStringLiteral("留空会在压缩时询问保存位置"));
        return;
    }
    const QFileInfo first(sources.first());
    QString base = QStringLiteral("StarZip_%1")
                       .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    if (sources.size() == 1) {
        const QString name = first.isDir() ? first.fileName() : first.completeBaseName();
        if (!name.isEmpty())
            base = name;
    }
    const QString dir = first.isDir() ? first.absolutePath() : first.absolutePath();
    m_outputEdit->setPlaceholderText(
        QStringLiteral("留空则使用：%1")
            .arg(QDir::toNativeSeparators(QDir(dir).filePath(base + choice.suffix))));
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

void MainWindow::fillArchiveTree(const QVector<ArchiveEntry> &entries)
{
    m_archiveTree->setUpdatesEnabled(false);
    m_archiveTree->clear();

    QVector<ArchiveEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const ArchiveEntry &a, const ArchiveEntry &b) {
        return a.path.compare(b.path, Qt::CaseInsensitive) < 0;
    });

    const QIcon dirIcon = style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon fileIcon = style()->standardIcon(QStyle::SP_FileIcon);
    QHash<QString, QTreeWidgetItem *> dirItems;

    std::function<QTreeWidgetItem *(const QString &)> ensureDir;
    ensureDir = [&](const QString &dirPath) -> QTreeWidgetItem * {
        const QString key = dirPath.toCaseFolded();
        auto found = dirItems.find(key);
        if (found != dirItems.end())
            return found.value();
        const int slash = dirPath.lastIndexOf(QLatin1Char('/'));
        QTreeWidgetItem *parent = nullptr;
        const QString name = slash >= 0 ? dirPath.mid(slash + 1) : dirPath;
        if (slash >= 0)
            parent = ensureDir(dirPath.left(slash));
        auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_archiveTree);
        item->setText(0, name);
        item->setIcon(0, dirIcon);
        item->setText(2, QStringLiteral("文件夹"));
        item->setData(0, Qt::UserRole, dirPath);
        dirItems.insert(key, item);
        return item;
    };

    for (const ArchiveEntry &entry : sorted) {
        QString path = entry.path;
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        while (path.endsWith(QLatin1Char('/')))
            path.chop(1);
        if (path.isEmpty())
            continue;

        const QString modified = entry.modified.isValid()
                                     ? entry.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                                     : QStringLiteral("—");

        QTreeWidgetItem *item = nullptr;
        if (entry.isDir) {
            item = ensureDir(path);
            item->setText(2, QStringLiteral("文件夹"));
        } else {
            const int slash = path.lastIndexOf(QLatin1Char('/'));
            const QString name = slash >= 0 ? path.mid(slash + 1) : path;
            QTreeWidgetItem *parent = slash >= 0 ? ensureDir(path.left(slash)) : nullptr;
            item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_archiveTree);
            item->setText(0, name);
            item->setIcon(0, fileIcon);
            item->setText(1, humanSize(entry.size));
            item->setText(2, QStringLiteral("文件"));
            item->setData(0, Qt::UserRole, path);
        }

        item->setData(0, Qt::UserRole + 1, entry.isDir);
        item->setText(3, modified);
        item->setText(4, entry.encrypted ? QStringLiteral("加密") : QString());
        item->setToolTip(0, path);
        if (entry.encrypted) {
            item->setForeground(4, QBrush(QColor(QStringLiteral("#b45309"))));
            item->setToolTip(4, QStringLiteral("该条目已加密，解压时需要密码"));
        }
        if (entry.isDir)
            item->setText(1, QStringLiteral("—"));
    }

    m_archiveTree->expandToDepth(0);
    m_archiveTree->setUpdatesEnabled(true);
    if (!m_filterEdit->text().isEmpty())
        filterArchiveEntries(m_filterEdit->text());
}

bool MainWindow::filterTreeItem(QTreeWidgetItem *item, const QString &needle)
{
    bool childVisible = false;
    for (int i = 0; i < item->childCount(); ++i)
        childVisible = filterTreeItem(item->child(i), needle) || childVisible;

    const bool selfMatch = needle.isEmpty()
                           || item->text(0).contains(needle, Qt::CaseInsensitive);
    const bool visible = needle.isEmpty() || selfMatch || childVisible;
    item->setHidden(!visible);
    if (!needle.isEmpty() && childVisible)
        item->setExpanded(true);
    return visible;
}

void MainWindow::filterArchiveEntries(const QString &text)
{
    const QString needle = text.trimmed();
    for (int i = 0; i < m_archiveTree->topLevelItemCount(); ++i)
        filterTreeItem(m_archiveTree->topLevelItem(i), needle);
}

void MainWindow::showArchiveContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_archiveTree->itemAt(pos);
    if (item && !item->isSelected()) {
        m_archiveTree->clearSelection();
        item->setSelected(true);
        m_archiveTree->setCurrentItem(item);
    }

    QMenu menu(this);
    QAction *extract = menu.addAction(QStringLiteral("解压选中项到当前目标…"));
    QAction *test = menu.addAction(QStringLiteral("测试压缩包"));
    menu.addSeparator();
    QAction *copy = menu.addAction(QStringLiteral("复制条目路径"));
    extract->setEnabled(!m_busy && m_archiveTree->selectedItems().size() > 0);
    test->setEnabled(!m_busy && !m_archivePath.isEmpty());
    copy->setEnabled(m_archiveTree->selectedItems().size() > 0);
    QAction *chosen = menu.exec(m_archiveTree->viewport()->mapToGlobal(pos));

    if (chosen == extract) {
        extractSelected();
    } else if (chosen == test) {
        testArchive();
    } else if (chosen == copy) {
        const QStringList paths = selectedArchivePaths();
        if (!paths.isEmpty())
            QApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
    }
}

QStringList MainWindow::selectedArchivePaths() const
{
    QStringList paths;
    for (QTreeWidgetItem *item : m_archiveTree->selectedItems()) {
        const QString path = item->data(0, Qt::UserRole).toString();
        if (!path.isEmpty())
            paths.append(path);
    }
    paths.removeDuplicates();
    return paths;
}

bool MainWindow::archiveHasEncryptedEntries() const
{
    for (const ArchiveEntry &entry : m_entries) {
        if (entry.encrypted)
            return true;
    }
    return false;
}

void MainWindow::onArchiveSelectionChanged()
{
    updateActions();
}

void MainWindow::showLogTab()
{
    m_tabs->setCurrentWidget(m_logPage);
}

void MainWindow::showAbout()
{
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("关于 StarZip"));
    box.setIcon(QMessageBox::Information);
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral(
        "<h3>StarZip 0.0.3</h3>"
        "<p>C++17 · Qt 6 Widgets · libarchive</p>"
        "<p>读取：ZIP / 7Z / RAR / RAR5 / TAR / GZ / XZ / CAB / ISO / UDF ISO<br>"
        "创建：ZIP / 7Z / TAR / TAR.GZ / TAR.XZ<br>"
        "密码：ZIP AES-256 写入；7Z 密码由 7-Zip 引擎写入；加密包读取时可输入密码<br>"
        "集成：Windows 资源管理器右键菜单 / 打开方式</p>"
        "<p>GitHub：<a href='https://github.com/wbzmm2025/StarZip'>"
        "github.com/wbzmm2025/StarZip</a></p>"
        "<p style='color:#6b7280'>0.0.3：右键菜单、UDF ISO、加密 7Z 与紧凑窗口。</p>"));
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();
}

void MainWindow::installShellIntegration()
{
#ifdef Q_OS_WIN
    QString error;
    if (ShellIntegration::install(&error)) {
        QSettings settings;
        settings.setValue(QStringLiteral("shellIntegration/autoInstalled"), true);
        settings.sync();
        appendLog(QStringLiteral("资源管理器右键菜单已安装/修复。"));
        QMessageBox::information(
            this, QStringLiteral("右键菜单已就绪"),
            QStringLiteral("已为当前用户添加 StarZip 菜单：\n\n"
                           "• 压缩包：使用 StarZip 打开 / 解压到当前文件夹 / 解压到…\n"
                           "• 任意文件或文件夹：使用 StarZip 压缩…\n\n"
                           "如果资源管理器仍显示旧菜单，请注销或重启资源管理器。"));
    } else {
        appendLog(QStringLiteral("安装右键菜单失败：%1").arg(error));
        showError(QStringLiteral("安装失败"), error);
    }
#else
    QMessageBox::information(this, QStringLiteral("不支持"),
                             QStringLiteral("资源管理器集成仅支持 Windows。"));
#endif
}

void MainWindow::removeShellIntegration()
{
#ifdef Q_OS_WIN
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, QStringLiteral("移除右键菜单"),
        QStringLiteral("确定移除当前用户的 StarZip 右键菜单和“打开方式”条目吗？\n"
                       "不会删除任何压缩包或程序文件。"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    QString error;
    if (ShellIntegration::uninstall(&error)) {
        QSettings settings;
        settings.setValue(QStringLiteral("shellIntegration/autoInstalled"), true);
        settings.sync();
        appendLog(QStringLiteral("资源管理器右键菜单已移除。"));
        QMessageBox::information(this, QStringLiteral("已移除"),
                                 QStringLiteral("当前用户的 StarZip 右键菜单已移除。"));
    } else {
        appendLog(QStringLiteral("移除右键菜单失败：%1").arg(error));
        showError(QStringLiteral("移除失败"), error);
    }
#else
    QMessageBox::information(this, QStringLiteral("不支持"),
                             QStringLiteral("资源管理器集成仅支持 Windows。"));
#endif
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

void MainWindow::onPasswordRequired(int operation, const QString &archivePath)
{
    Q_UNUSED(operation)
    Q_UNUSED(archivePath)
    m_passwordSignal = true;
    appendLog(QStringLiteral("压缩包需要密码或密码不正确，等待输入。"));
    if (m_busy) {
        m_progressLabel->setText(QStringLiteral("等待输入密码…"));
        statusBar()->showMessage(QStringLiteral("压缩包已加密，等待输入密码…"));
    }
}

void MainWindow::onArchiveListed(const QVector<ArchiveEntry> &entries,
                                 const QString &formatName, qint64 totalBytes, int entryCount,
                                 bool truncated)
{
    m_entries = entries;
    m_formatName = formatName.isEmpty() ? QStringLiteral("未知格式") : formatName;
    fillArchiveTree(entries);
    updateArchiveHeader(m_formatName, totalBytes, entryCount, truncated);
    m_archiveStack->setCurrentWidget(m_contentPage);
    m_emptyTitle->setText(QStringLiteral("打开或拖入一个压缩包"));
    m_emptyHint->setText(QStringLiteral("支持从本地浏览选择，也可以直接把压缩包拖到窗口。\n"
                                        "ZIP / 7Z / RAR 的解压、列表、进度与密码提示保持一致。"));
    m_emptyFormats->setText(
        QStringLiteral("ZIP · 7Z · RAR · RAR5 · TAR · GZ · XZ · CAB · ISO"));
    m_tabs->setCurrentWidget(m_archivePage);
    addRecentFile(m_archivePath);
    updateStatusInfo();

    QString log = QStringLiteral("识别完成：格式 %1，%2 个条目，解压后 %3%4")
                      .arg(m_formatName)
                      .arg(entryCount)
                      .arg(humanSize(totalBytes))
                      .arg(truncated ? QStringLiteral("（列表已截断）") : QString());
    if (archiveHasEncryptedEntries())
        log += QStringLiteral("；包含加密条目，解压时需要密码。");
    appendLog(log);
}

void MainWindow::onWorkerFinished(int operation, bool success, bool canceled,
                                  const QString &message, int errorCode,
                                  const QString &outputPath)
{
    const bool passwordIssue = m_passwordSignal;
    m_passwordSignal = false;
    m_busy = false;
    cleanupThread();
    m_worker.clear();
    m_progressBar->setRange(0, 100);
    m_cancelButton->setEnabled(false);
    m_cancelAction->setEnabled(false);

    if (canceled) {
        m_progressBar->setValue(0);
        m_progressLabel->setText(QStringLiteral("已取消"));
        if (operation == ArchiveWorker::List) {
            m_startupAction = StartupAction::None;
            resetArchiveState();
        }
        statusBar()->showMessage(QStringLiteral("操作已取消。"), 6000);
        appendLog(QStringLiteral("操作已取消。"));
        updateActions();
        return;
    }

    if (!success) {
        m_progressBar->setValue(0);
        if (errorCode != 0)
            appendLog(QStringLiteral("错误码：%1").arg(errorCode));
        appendLog(message);

        if (passwordIssue) {
            if (m_passwordAttempts >= 3) {
                showError(QStringLiteral("密码错误"),
                          message + QStringLiteral("\n\n已连续尝试 3 次。"));
                if (operation == ArchiveWorker::List)
                    resetArchiveState();
                updateActions();
                return;
            }
            const QString password = promptPassword(
                QStringLiteral("需要密码"),
                QStringLiteral("该压缩包已加密，或上次输入的密码不正确。\n"
                               "请输入密码后重试："));
            if (!password.isEmpty()) {
                ++m_passwordAttempts;
                m_archivePassword = password;
                if (operation == ArchiveWorker::List) {
                    const QString path = m_archivePath;
                    openArchive(path, password, true);
                } else if (operation == ArchiveWorker::Extract) {
                    startExtraction(m_lastExtractDest, m_lastExtractOnly, password, true);
                } else if (operation == ArchiveWorker::Test) {
                    startTest(password, true);
                }
                updateActions();
                return;
            }
            statusBar()->showMessage(QStringLiteral("已取消密码输入。"), 6000);
            appendLog(QStringLiteral("用户取消了密码输入。"));
            m_progressLabel->setText(QStringLiteral("已取消"));
            if (operation == ArchiveWorker::List)
                resetArchiveState();
            updateActions();
            return;
        }

        if (operation == ArchiveWorker::List) {
            resetArchiveState();
            m_progressLabel->setText(QStringLiteral("失败"));
        }
        showError(operationLabel(operation) + QStringLiteral("失败"), message);
        updateActions();
        return;
    }

    m_passwordAttempts = 0;

    if (operation == ArchiveWorker::List) {
        statusBar()->showMessage(message, 8000);
        m_progressLabel->setText(QStringLiteral("就绪"));
        appendLog(message);
        if (m_startupAction != StartupAction::None)
            QTimer::singleShot(0, this, &MainWindow::runStartupAction);
    } else {
        m_progressBar->setValue(100);
        m_progressLabel->setText(QStringLiteral("完成"));
        appendLog(message);
        QString folder;
        if (operation == ArchiveWorker::Extract)
            folder = outputPath;
        else if (operation == ArchiveWorker::Compress)
            folder = QFileInfo(outputPath).absolutePath();
        showSuccess(operationLabel(operation) + QStringLiteral("成功"), message, folder);
    }
    updateActions();
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
    m_passwordSignal = false;
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
    connect(worker, &ArchiveWorker::passwordRequired, this, &MainWindow::onPasswordRequired,
            Qt::QueuedConnection);
    connect(worker, &ArchiveWorker::finished, this, &MainWindow::onWorkerFinished,
            Qt::QueuedConnection);
    connect(worker, &ArchiveWorker::finished, thread, &QThread::quit);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);

    thread->start();
    return true;
}

void MainWindow::cleanupThread()
{
    if (m_thread && m_thread->isRunning()) {
        m_thread->quit();
        m_thread->wait(5000);
    }
}

void MainWindow::resetArchiveState()
{
    m_archivePath.clear();
    m_archivePassword.clear();
    m_formatName.clear();
    m_entries.clear();
    m_startupAction = StartupAction::None;
    m_passwordAttempts = 0;
    m_passwordSignal = false;
    if (m_archiveTree)
        m_archiveTree->clear();
    if (m_archiveStack)
        m_archiveStack->setCurrentWidget(m_emptyPage);
    if (m_emptyTitle)
        m_emptyTitle->setText(QStringLiteral("打开或拖入一个压缩包"));
    if (m_emptyHint) {
        m_emptyHint->setText(QStringLiteral("支持从本地浏览选择，也可以直接把压缩包拖到窗口。\n"
                                            "ZIP / 7Z / RAR 的解压、列表、进度与密码提示保持一致。"));
    }
    if (m_emptyFormats) {
        m_emptyFormats->setText(
            QStringLiteral("ZIP · 7Z · RAR · RAR5 · TAR · GZ · XZ · CAB · ISO"));
    }
    m_archiveLabel->setText(QStringLiteral("未选择"));
    m_archiveLabel->setToolTip(QString());
    m_archiveInfoLabel->setText(QStringLiteral("尚未打开压缩包。"));
    m_formatBadge->setText(QStringLiteral("—"));
    m_statusFormatLabel->setText(QStringLiteral("格式：—"));
    m_statusCountLabel->setText(QStringLiteral("条目：—"));
    setWindowTitle(QStringLiteral("StarZip 0.0.3"));
    updateActions();
}

void MainWindow::updateActions()
{
    // createCompressPage() 会在 setupUi() 仍未完成时触发本函数，
    // 此时进度栏/取消按钮尚未创建，直接返回避免访问空指针。
    if (!m_uiReady)
        return;
    const bool idle = !m_busy;
    const bool hasArchive = idle && !m_archivePath.isEmpty();
    const bool hasSelection = hasArchive && m_archiveTree
                              && !m_archiveTree->selectedItems().isEmpty();
    const bool hasSources = m_sourcesTree && m_sourcesTree->topLevelItemCount() > 0;

    m_openAction->setEnabled(idle);
    m_newAction->setEnabled(idle);
    m_openButton->setEnabled(idle);
    m_emptyOpenButton->setEnabled(idle);
    m_emptyNewButton->setEnabled(idle);
    m_browseTargetButton->setEnabled(idle);
    m_targetEdit->setEnabled(idle);
    m_extractHereAction->setEnabled(hasArchive);
    m_extractToAction->setEnabled(hasArchive);
    m_extractHereButton->setEnabled(hasArchive);
    m_extractToButton->setEnabled(hasArchive);
    m_extractSelectedAction->setEnabled(hasSelection);
    m_extractSelectedButton->setEnabled(hasSelection);
    m_testAction->setEnabled(hasArchive);
    m_testButton->setEnabled(hasArchive);
    m_filterEdit->setEnabled(hasArchive);
    m_cancelAction->setEnabled(m_busy);
    m_cancelButton->setEnabled(m_busy);

    m_addFilesAction->setEnabled(idle);
    m_addFolderAction->setEnabled(idle);
    m_addFilesButton->setEnabled(idle);
    m_addFolderButton->setEnabled(idle);
    m_removeSourceButton->setEnabled(idle && m_sourcesTree
                                     && !m_sourcesTree->selectedItems().isEmpty());
    m_clearSourcesButton->setEnabled(idle && hasSources);
    m_compressAction->setEnabled(idle && hasSources);
    m_createArchiveButton->setEnabled(idle && hasSources);
    m_formatCombo->setEnabled(idle);
    m_levelCombo->setEnabled(idle && currentFormatChoice().supportsLevel);
    m_passwordEdit->setEnabled(idle && currentFormatChoice().supportsPassword);
    m_showPasswordCheck->setEnabled(idle && currentFormatChoice().supportsPassword);
    m_outputEdit->setEnabled(idle);
    m_browseOutputButton->setEnabled(idle);
}

void MainWindow::updateStatusInfo()
{
    QString badge = m_formatName;
    if (badge.contains(QStringLiteral("7-Zip"), Qt::CaseInsensitive))
        badge = QStringLiteral("7Z");
    else if (badge.contains(QStringLiteral("ZIP"), Qt::CaseInsensitive))
        badge = QStringLiteral("ZIP");
    else if (badge.contains(QStringLiteral("RAR"), Qt::CaseInsensitive))
        badge = QStringLiteral("RAR");
    else if (badge.contains(QStringLiteral("TAR"), Qt::CaseInsensitive))
        badge = QStringLiteral("TAR");
    else if (badge.isEmpty())
        badge = QStringLiteral("—");
    m_statusFormatLabel->setText(QStringLiteral("格式：%1").arg(badge));
    m_statusCountLabel->setText(QStringLiteral("条目：%1").arg(m_entries.size()));
}

void MainWindow::updateArchiveHeader(const QString &formatName, qint64 totalBytes,
                                     int entryCount, bool truncated)
{
    if (formatName.isEmpty()) {
        m_formatBadge->setText(QStringLiteral("—"));
        m_archiveInfoLabel->setText(QStringLiteral("尚未打开压缩包。"));
        return;
    }

    QString badge = QStringLiteral("ZIP");
    if (formatName.contains(QStringLiteral("7-Zip"), Qt::CaseInsensitive))
        badge = QStringLiteral("7Z");
    else if (formatName.contains(QStringLiteral("RAR"), Qt::CaseInsensitive))
        badge = QStringLiteral("RAR");
    else if (formatName.contains(QStringLiteral("TAR"), Qt::CaseInsensitive))
        badge = QStringLiteral("TAR");
    else if (formatName.contains(QStringLiteral("ZIP"), Qt::CaseInsensitive))
        badge = QStringLiteral("ZIP");
    else
        badge = formatName.section(QLatin1Char(' '), 0, 0).toUpper();
    m_formatBadge->setText(badge);
    m_formatBadge->setToolTip(formatName);

    int encrypted = 0;
    for (const ArchiveEntry &entry : m_entries) {
        if (entry.encrypted)
            ++encrypted;
    }
    QString info = QStringLiteral("格式：%1    条目：%2    解压后大小：%3")
                       .arg(formatName)
                       .arg(entryCount)
                       .arg(humanSize(totalBytes));
    if (encrypted > 0)
        info += QStringLiteral("    加密条目：%1").arg(encrypted);
    if (truncated)
        info += QStringLiteral("    列表仅显示前 20000 项");
    m_archiveInfoLabel->setText(info);
    m_statusFormatLabel->setText(QStringLiteral("格式：%1").arg(badge));
    m_statusCountLabel->setText(QStringLiteral("条目：%1").arg(entryCount));
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

void MainWindow::showSuccess(const QString &title, const QString &message,
                             const QString &folderPath)
{
    statusBar()->showMessage(QStringLiteral("%1：%2").arg(title, firstLine(message)), 10000);
    QMessageBox box(this);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(title);
    box.setText(title);
    box.setInformativeText(message);
    QPushButton *openFolder = nullptr;
    if (!folderPath.isEmpty())
        openFolder = box.addButton(QStringLiteral("打开文件夹"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Ok);
    box.exec();
    if (openFolder && box.clickedButton() == openFolder)
        QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath));
}

QString MainWindow::promptPassword(const QString &title, const QString &text)
{
    bool ok = false;
    const QString password = QInputDialog::getText(
        this, title, text, QLineEdit::Password, QString(), &ok);
    if (!ok)
        return QString();
    return password;
}

void MainWindow::cancelCurrentTask()
{
    if (!m_busy || !m_worker)
        return;
    m_worker->requestCancel();
    m_cancelButton->setEnabled(false);
    m_cancelAction->setEnabled(false);
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
    case ArchiveWorker::Test:
        return QStringLiteral("测试压缩包");
    default:
        return QStringLiteral("操作");
    }
}

QString MainWindow::firstLine(const QString &text)
{
    return text.section(QLatin1Char('\n'), 0, 0).left(220);
}

QString MainWindow::archiveDisplayName(const QString &path)
{
    const QFileInfo info(path);
    return info.fileName().isEmpty() ? path : info.fileName();
}
