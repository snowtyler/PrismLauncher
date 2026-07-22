#pragma once

#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QPropertyAnimation>
#include <QTimer>
#include <functional>

class ToastNotification : public QFrame {
    Q_OBJECT
public:
    explicit ToastNotification(QWidget* parent = nullptr);
    virtual ~ToastNotification() = default;

    void showToast(const QString& title,
                   const QString& message,
                   const QString& buttonText = QString(),
                   std::function<void()> onButtonClicked = nullptr,
                   int durationMs = 7000);

    void hideToast();

signals:
    void actionClicked();
    void dismissed();

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QLabel* m_iconLabel = nullptr;
    QLabel* m_titleLabel = nullptr;
    QLabel* m_messageLabel = nullptr;
    QPushButton* m_actionButton = nullptr;
    QToolButton* m_closeButton = nullptr;

    QPropertyAnimation* m_slideAnim = nullptr;
    QTimer* m_autoHideTimer = nullptr;
    std::function<void()> m_actionCallback = nullptr;
};
