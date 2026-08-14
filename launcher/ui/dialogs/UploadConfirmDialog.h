#pragma once
#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QTreeView>
#include <QFileSystemModel>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QPushButton>
#include <BaseInstance.h>
#include <FileIgnoreProxy.h>
#include <FastFileIconProvider.h>

class UploadConfirmDialog : public QDialog {
    Q_OBJECT
public:
    QLineEdit* shortcodeEdit;
    QLineEdit* descriptionEdit;
    QLineEdit* versionEdit;
    QCheckBox* privateCheck;
    QCheckBox* forceConfigOverwriteCheck;
    QCheckBox* overrideMemoryCheck;
    QSpinBox* minMemSpin;
    QSpinBox* maxMemSpin;
    QCheckBox* overrideJavaArgsCheck;
    QLineEdit* jvmArgsEdit;
    QLineEdit* bannerImageEdit;
    QPushButton* bannerBrowseBtn;
    QString bannerImagePath;
    QLineEdit* accessKeyEdit;
    QLineEdit* secretKeyEdit;
    QLineEdit* endpointEdit;
    QLineEdit* bucketEdit;
    QLineEdit* publicUrlEdit;
    QTreeView* treeView;
    FileIgnoreProxy* proxyModel;
    BaseInstance* m_instance;
    FastFileIconProvider m_icons;

    explicit UploadConfirmDialog(BaseInstance* inst, QWidget* parent = nullptr);
    void accept() override;

private slots:
    void rowsInserted(QModelIndex parent, int top, int bottom);

private:
    QString ignoreFileName() const;
};
