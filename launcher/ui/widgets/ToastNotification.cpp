#include "ToastNotification.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QIcon>
#include <QStyle>
#include <QEasingCurve>

ToastNotification::ToastNotification(QWidget* parent)
    : QFrame(parent)
{
    setObjectName("ToastNotification");
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_DeleteOnClose, false);

    // Styling
    setStyleSheet(R"(
        #ToastNotification {
            background-color: #1E222D;
            border: 1px solid #374151;
            border-radius: 8px;
        }
        QLabel#ToastTitle {
            color: #FFFFFF;
            font-weight: bold;
            font-size: 13px;
        }
        QLabel#ToastMessage {
            color: #D1D5DB;
            font-size: 12px;
        }
        QPushButton#ToastActionButton {
            background-color: #2563EB;
            color: #FFFFFF;
            font-weight: bold;
            font-size: 12px;
            border: none;
            border-radius: 4px;
            padding: 5px 12px;
        }
        QPushButton#ToastActionButton:hover {
            background-color: #1D4ED8;
        }
        QToolButton#ToastCloseButton {
            color: #9CA3AF;
            background: transparent;
            border: none;
            font-size: 14px;
            font-weight: bold;
        }
        QToolButton#ToastCloseButton:hover {
            color: #FFFFFF;
        }
    )");

    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(12, 10, 12, 10);
    mainLayout->setSpacing(10);

    // Icon
    m_iconLabel = new QLabel(this);
    m_iconLabel->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxInformation).pixmap(24, 24));
    mainLayout->addWidget(m_iconLabel, 0, Qt::AlignTop);

    // Text Container
    auto* textLayout = new QVBoxLayout();
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(2);

    m_titleLabel = new QLabel(this);
    m_titleLabel->setObjectName("ToastTitle");
    textLayout->addWidget(m_titleLabel);

    m_messageLabel = new QLabel(this);
    m_messageLabel->setObjectName("ToastMessage");
    m_messageLabel->setWordWrap(true);
    textLayout->addWidget(m_messageLabel);

    mainLayout->addLayout(textLayout, 1);

    // Action Button
    m_actionButton = new QPushButton(this);
    m_actionButton->setObjectName("ToastActionButton");
    m_actionButton->setCursor(Qt::PointingHandCursor);
    m_actionButton->hide();
    mainLayout->addWidget(m_actionButton, 0, Qt::AlignVCenter);

    connect(m_actionButton, &QPushButton::clicked, this, [this]() {
        if (m_actionCallback) {
            m_actionCallback();
        }
        emit actionClicked();
        hideToast();
    });

    // Close Button
    m_closeButton = new QToolButton(this);
    m_closeButton->setObjectName("ToastCloseButton");
    m_closeButton->setText("✕");
    m_closeButton->setCursor(Qt::PointingHandCursor);
    mainLayout->addWidget(m_closeButton, 0, Qt::AlignTop);

    connect(m_closeButton, &QToolButton::clicked, this, &ToastNotification::hideToast);

    m_autoHideTimer = new QTimer(this);
    m_autoHideTimer->setSingleShot(true);
    connect(m_autoHideTimer, &QTimer::timeout, this, &ToastNotification::hideToast);

    hide();
}

void ToastNotification::showToast(const QString& title,
                                   const QString& message,
                                   const QString& buttonText,
                                   std::function<void()> onButtonClicked,
                                   int durationMs)
{
    m_titleLabel->setText(title);
    m_messageLabel->setText(message);
    m_actionCallback = onButtonClicked;

    if (!buttonText.isEmpty()) {
        m_actionButton->setText(buttonText);
        m_actionButton->show();
    } else {
        m_actionButton->hide();
    }

    if (parentWidget()) {
        setFixedWidth(qMin(380, qMax(260, parentWidget()->width() - 40)));
    }
    adjustSize();

    int targetX = parentWidget() ? qMax(10, parentWidget()->width() - width() - 25) : 10;
    int targetY = parentWidget() ? qMax(10, parentWidget()->height() - height() - 25) : 400;
    int startY = parentWidget() ? parentWidget()->height() + 20 : 600;

    move(targetX, startY);
    show();
    raise();

    // Slide-in animation (Up from bottom right)
    if (m_slideAnim) {
        m_slideAnim->stop();
        m_slideAnim->deleteLater();
    }
    m_slideAnim = new QPropertyAnimation(this, "pos", this);
    m_slideAnim->setDuration(300);
    m_slideAnim->setEasingCurve(QEasingCurve::OutCubic);
    m_slideAnim->setStartValue(QPoint(targetX, startY));
    m_slideAnim->setEndValue(QPoint(targetX, targetY));
    m_slideAnim->start();

    if (durationMs > 0) {
        m_autoHideTimer->start(durationMs);
    }
}

void ToastNotification::hideToast()
{
    m_autoHideTimer->stop();

    if (m_slideAnim) {
        m_slideAnim->stop();
        m_slideAnim->deleteLater();
    }

    int currentX = x();
    int startY = y();
    int endY = parentWidget() ? parentWidget()->height() + 20 : startY + height() + 20;

    m_slideAnim = new QPropertyAnimation(this, "pos", this);
    m_slideAnim->setDuration(200);
    m_slideAnim->setEasingCurve(QEasingCurve::InCubic);
    m_slideAnim->setStartValue(QPoint(currentX, startY));
    m_slideAnim->setEndValue(QPoint(currentX, endY));

    connect(m_slideAnim, &QPropertyAnimation::finished, this, [this]() {
        hide();
        emit dismissed();
    });

    m_slideAnim->start();
}

void ToastNotification::enterEvent(QEnterEvent* event)
{
    QFrame::enterEvent(event);
    m_autoHideTimer->stop();
}

void ToastNotification::leaveEvent(QEvent* event)
{
    QFrame::leaveEvent(event);
    m_autoHideTimer->start(3000);
}
