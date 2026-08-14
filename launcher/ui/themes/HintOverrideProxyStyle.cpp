// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2024 TheKodeToad <TheKodeToad@proton.me>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "HintOverrideProxyStyle.h"

#include <QPainter>
#include <QPaintDevice>
#include <QStyleOptionToolButton>

HintOverrideProxyStyle::HintOverrideProxyStyle(QStyle* style) : QProxyStyle(style)
{
    setObjectName(baseStyle()->objectName());
}

int HintOverrideProxyStyle::styleHint(QStyle::StyleHint hint,
                                      const QStyleOption* option,
                                      const QWidget* widget,
                                      QStyleHintReturn* returnData) const
{
    if (hint == QStyle::SH_ItemView_ActivateItemOnSingleClick)
        return 0;

    if (hint == QStyle::SH_Slider_AbsoluteSetButtons)
        return Qt::LeftButton | Qt::MiddleButton;

    if (hint == QStyle::SH_Slider_PageSetButtons)
        return Qt::RightButton;

    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

static constexpr int ToolButtonExtraSpacing = 6;

QSize HintOverrideProxyStyle::sizeFromContents(ContentsType type,
                                               const QStyleOption* option,
                                               const QSize& contentsSize,
                                               const QWidget* widget) const
{
    QSize size = QProxyStyle::sizeFromContents(type, option, contentsSize, widget);
    if (type == CT_ToolButton) {
        if (const auto* toolbutton = qstyleoption_cast<const QStyleOptionToolButton*>(option)) {
            if (toolbutton->toolButtonStyle == Qt::ToolButtonTextBesideIcon
                && !toolbutton->text.isEmpty()
                && (!toolbutton->icon.isNull() || (toolbutton->features & QStyleOptionToolButton::Arrow))) {
                size.rwidth() += ToolButtonExtraSpacing;
            }
        }
    }
    return size;
}

void HintOverrideProxyStyle::drawControl(ControlElement element,
                                         const QStyleOption* option,
                                         QPainter* painter,
                                         const QWidget* widget) const
{
    if (element == CE_ToolButtonLabel) {
        if (const auto* toolbutton = qstyleoption_cast<const QStyleOptionToolButton*>(option)) {
            if (toolbutton->toolButtonStyle == Qt::ToolButtonTextBesideIcon
                && !toolbutton->text.isEmpty()
                && (!toolbutton->icon.isNull() || (toolbutton->features & QStyleOptionToolButton::Arrow))) {

                QRect rect = toolbutton->rect;
                int shiftX = 0;
                int shiftY = 0;
                if (toolbutton->state & (State_Sunken | State_On)) {
                    shiftX = proxy()->pixelMetric(PM_ButtonShiftHorizontal, toolbutton, widget);
                    shiftY = proxy()->pixelMetric(PM_ButtonShiftVertical, toolbutton, widget);
                }

                bool hasArrow = (toolbutton->features & QStyleOptionToolButton::Arrow);
                QPixmap pm;
                QSize pmSize = toolbutton->iconSize;
                if (!hasArrow && !toolbutton->icon.isNull()) {
                    QIcon::State state = toolbutton->state & State_On ? QIcon::On : QIcon::Off;
                    QIcon::Mode mode;
                    if (!(toolbutton->state & State_Enabled))
                        mode = QIcon::Disabled;
                    else if ((toolbutton->state & State_MouseOver) && (toolbutton->state & State_AutoRaise))
                        mode = QIcon::Active;
                    else
                        mode = QIcon::Normal;

                    qreal dpr = painter->device() ? painter->device()->devicePixelRatio() : 1.0;
                    pm = toolbutton->icon.pixmap(toolbutton->rect.size().boundedTo(toolbutton->iconSize),
                                                 dpr,
                                                 mode, state);
                    pmSize = pm.size() / pm.devicePixelRatio();
                }

                painter->setFont(toolbutton->font);

                int alignment = Qt::TextShowMnemonic;
                if (!proxy()->styleHint(SH_UnderlineShortcut, toolbutton, widget))
                    alignment |= Qt::TextHideMnemonic;

                QRect pr = rect;
                QRect tr = rect;
                pr.setWidth(pmSize.width() + 4);
                tr.adjust(pr.width() + ToolButtonExtraSpacing, 0, 0, 0);

                pr.translate(shiftX, shiftY);
                if (!hasArrow) {
                    proxy()->drawItemPixmap(painter, visualRect(option->direction, rect, pr), Qt::AlignCenter, pm);
                } else {
                    QStyle::PrimitiveElement pe = QStyle::PE_IndicatorArrowDown;
                    switch (toolbutton->arrowType) {
                        case Qt::LeftArrow:
                            pe = QStyle::PE_IndicatorArrowLeft;
                            break;
                        case Qt::RightArrow:
                            pe = QStyle::PE_IndicatorArrowRight;
                            break;
                        case Qt::UpArrow:
                            pe = QStyle::PE_IndicatorArrowUp;
                            break;
                        case Qt::DownArrow:
                            pe = QStyle::PE_IndicatorArrowDown;
                            break;
                        default:
                            break;
                    }
                    QStyleOptionToolButton arrow = *toolbutton;
                    arrow.rect = pr;
                    proxy()->drawPrimitive(pe, &arrow, painter, widget);
                }

                alignment |= Qt::AlignLeft | Qt::AlignVCenter;
                tr.translate(shiftX, shiftY);

                QString text = toolbutton->text;
                if (toolbutton->fontMetrics.horizontalAdvance(text) > tr.width()) {
                    text = toolbutton->fontMetrics.elidedText(text, Qt::ElideMiddle, tr.width(), alignment);
                }

                proxy()->drawItemText(painter, visualRect(option->direction, rect, tr), alignment, toolbutton->palette,
                                      toolbutton->state & State_Enabled, text, QPalette::ButtonText);
                return;
            }
        }
    }
    QProxyStyle::drawControl(element, option, painter, widget);
}
