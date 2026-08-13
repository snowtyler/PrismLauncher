#include "UploadConfirmDialog.h"
#include "Application.h"
#include "FileSystem.h"
#include <QDir>
#include <QHeaderView>
#include <QFileDialog>

static QString bumpVersion(const QString& version) {
    QStringList parts = version.split('.');
    if (parts.isEmpty()) return "1.0.0";
    bool ok;
    int last = parts.last().toInt(&ok);
    if (ok) {
        QStringList result = parts;
        result.last() = QString::number(last + 1);
        return result.join('.');
    }
    return version;
}

UploadConfirmDialog::UploadConfirmDialog(BaseInstance* inst, QWidget* parent)
    : QDialog(parent), m_instance(inst)
{
    setWindowTitle(tr("Upload / Sync Modpack"));
    setMinimumSize(520, 720);

    auto* mainLayout = new QVBoxLayout(this);

    // Version & Private inputs
    auto* formLayout = new QFormLayout();
    
    QString currentShortcode = inst->settings()->get("SyncShortcode").toString();
    shortcodeEdit = new QLineEdit(currentShortcode, this);
    shortcodeEdit->setPlaceholderText(tr("e.g. COZYPACK"));
    formLayout->addRow(tr("Sync Shortcode:"), shortcodeEdit);

    QString currentDesc = inst->settings()->get("ExportSummary").toString();
    descriptionEdit = new QLineEdit(currentDesc, this);
    descriptionEdit->setPlaceholderText(tr("Leave empty to keep existing description on server"));
    formLayout->addRow(tr("Modpack Description:"), descriptionEdit);

    QString currentVer = inst->settings()->get("SyncVersion").toString();
    QString nextVer;
    if (currentVer.isEmpty()) {
        nextVer = "1.0.0";
    } else {
        nextVer = bumpVersion(currentVer);
    }
    versionEdit = new QLineEdit(nextVer, this);
    formLayout->addRow(tr("New Pack Version:"), versionEdit);

    // Custom Banner Image field
    auto* bannerLayout = new QHBoxLayout();
    bannerImageEdit = new QLineEdit(this);
    bannerImageEdit->setReadOnly(true);
    bannerImageEdit->setPlaceholderText(tr("Click Browse to upload a custom banner..."));
    bannerLayout->addWidget(bannerImageEdit);

    bannerBrowseBtn = new QPushButton(tr("Browse..."), this);
    bannerLayout->addWidget(bannerBrowseBtn);
    formLayout->addRow(tr("Banner Image (ideal: 270x135px):"), bannerLayout);

    connect(bannerBrowseBtn, &QPushButton::clicked, this, [this]() {
        QString file = QFileDialog::getOpenFileName(this, tr("Select Banner Image"), "", tr("Images (*.png *.jpg *.jpeg)"));
        if (!file.isEmpty()) {
            bannerImagePath = file;
            bannerImageEdit->setText(file);
        }
    });

    privateCheck = new QCheckBox(tr("Private Modpack (Hidden from discovery)"), this);
    privateCheck->setChecked(inst->settings()->get("SyncIsPrivate").toBool());
    formLayout->addRow(privateCheck);

    forceConfigOverwriteCheck = new QCheckBox(tr("Force client config update (overwrites options.txt)"), this);
    forceConfigOverwriteCheck->setToolTip(tr("When checked, clients will be forced to redownload options.txt during this update instead of keeping local changes."));
    forceConfigOverwriteCheck->setChecked(false);
    formLayout->addRow(forceConfigOverwriteCheck);

    mainLayout->addLayout(formLayout);

    // File selection checklist tree view
    mainLayout->addWidget(new QLabel(tr("Select files and folders to include in sync:"), this));
    treeView = new QTreeView(this);
    auto* model = new QFileSystemModel(this);
    model->setIconProvider(&m_icons);
    QString root = inst->instanceRoot();
    proxyModel = new FileIgnoreProxy(root, this);
    proxyModel->setSourceModel(model);

    // Exclude logs, crash-reports, .cache, .fabric, .quilt, .mixin.out
    QString prefix = QDir(root).relativeFilePath(inst->gameRoot());
    for (auto path : { "logs", "crash-reports", ".cache", ".fabric", ".quilt", ".mixin.out" }) {
        proxyModel->ignoreFilesWithPath().insert(FS::PathCombine(prefix, path));
    }
    proxyModel->ignoreFilesWithName().append({ ".DS_Store", "thumbs.db", "Thumbs.db" });

    // Load standard selection ignore files (retains previously selected set)
    proxyModel->loadBlockedPathsFromFile(ignoreFileName());

    treeView->setModel(proxyModel);

    // Set root path FIRST on model to constrain model scope strictly to instance root
    model->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs | QDir::Hidden);
    QModelIndex rootIdx = model->setRootPath(root);
    treeView->setRootIndex(proxyModel->mapFromSource(rootIdx));

    // Also update root index when directory loaded signal completes to prevent showing drive root
    connect(model, &QFileSystemModel::directoryLoaded, this, [this, model, root]() {
        QModelIndex srcIdx = model->index(root);
        if (srcIdx.isValid()) {
            treeView->setRootIndex(proxyModel->mapFromSource(srcIdx));
        }
    });

    connect(proxyModel, &QAbstractItemModel::rowsInserted, this, &UploadConfirmDialog::rowsInserted);

    treeView->sortByColumn(0, Qt::AscendingOrder);
    treeView->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    treeView->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    mainLayout->addWidget(treeView);

    // Credentials
    auto* credForm = new QFormLayout();
    accessKeyEdit = new QLineEdit(APPLICATION->settings()->get("SyncR2AccessKey").toString(), this);
    credForm->addRow(tr("R2 Access Key:"), accessKeyEdit);

    secretKeyEdit = new QLineEdit(APPLICATION->settings()->get("SyncR2SecretKey").toString(), this);
    secretKeyEdit->setEchoMode(QLineEdit::Password);
    credForm->addRow(tr("R2 Secret Key:"), secretKeyEdit);
    mainLayout->addLayout(credForm);

    // Advanced Connection Settings Group
    auto* advGroup = new QGroupBox(tr("Advanced Connection Settings (R2/S3)"), this);
    auto* advLayout = new QFormLayout(advGroup);

    endpointEdit = new QLineEdit(APPLICATION->settings()->get("SyncR2Endpoint").toString(), this);
    advLayout->addRow(tr("Endpoint URL:"), endpointEdit);

    bucketEdit = new QLineEdit(APPLICATION->settings()->get("SyncR2Bucket").toString(), this);
    advLayout->addRow(tr("Bucket Name:"), bucketEdit);

    publicUrlEdit = new QLineEdit(APPLICATION->settings()->get("SyncR2PublicUrl").toString(), this);
    advLayout->addRow(tr("Public URL:"), publicUrlEdit);

    mainLayout->addWidget(advGroup);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void UploadConfirmDialog::rowsInserted(QModelIndex parent, int top, int bottom)
{
    for (int i = top; i < bottom; i++) {
        auto node = proxyModel->index(i, 0, parent);
        if (proxyModel->shouldExpand(node)) {
            treeView->expand(node);
        }
    }
}

QString UploadConfirmDialog::ignoreFileName() const
{
    return FS::PathCombine(m_instance->instanceRoot(), ".syncignore");
}

void UploadConfirmDialog::accept()
{
    proxyModel->saveBlockedPathsToFile(ignoreFileName());
    QDialog::accept();
}
