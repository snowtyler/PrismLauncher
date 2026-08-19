/// SPDX-License-Identifier: GPL-3.0-only
/*
 *  PrismLaucher - Minecraft Launcher
 *  Copyright (C) 2023 Rachel Powers <508861+Ryex@users.noreply.github.com>
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
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *      Copyright 2013-2021 MultiMC Contributors
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#include "ProgressDialog.h"
#include <QPoint>
#include "ui_ProgressDialog.h"

#include <QDebug>
#include <QKeyEvent>
#include <limits>

#include "tasks/Task.h"

#include "ui/widgets/SubTaskProgressBar.h"

// map a value in a numeric range of an arbitrary type to between 0 and 10000
// for high precision (0.01%) without overflowing Qt's internal integer arithmetic
template <typename T, std::enable_if_t<std::is_arithmetic_v<T>, bool> = true>
std::tuple<int, int> map_int_zero_max(T current, T range_max, T range_min)
{
    constexpr int scale = 10000;
    auto type_range = range_max - range_min;
    if (type_range <= 0) {
        return { 0, scale };
    }

    double percentage = static_cast<double>(current - range_min) / static_cast<double>(type_range);
    percentage = std::clamp(percentage, 0.0, 1.0);
    int mapped_current = static_cast<int>(percentage * scale);

    return { mapped_current, scale };
}

ProgressDialog::ProgressDialog(QWidget* parent) : QDialog(parent), ui(new Ui::ProgressDialog)
{
    ui->setupUi(this);
    ui->detailsContainer->setVisible(false);
    ui->taskProgressScrollArea->setVisible(false);
    this->setWindowFlags(this->windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setAttribute(Qt::WidgetAttribute::WA_QuitOnClose, true);
    changeProgress(0, 100);
    setSkipButton(false);

    ui->detailsButton->setStyleSheet(R"(
        QPushButton {
            color: #9CA3AF;
            font-size: 11px;
            text-align: left;
            padding: 2px 4px;
            border: none;
            background: transparent;
        }
        QPushButton:hover {
            color: #E2E8F0;
            text-decoration: underline;
        }
    )");
    ui->globalStatusDetailsLabel->setStyleSheet("color: #9CA3AF; font-size: 11px; padding: 4px 0px;");

    updateSize(true);
}

void ProgressDialog::setSkipButton(bool present, QString label)
{
    ui->skipButton->setAutoDefault(false);
    ui->skipButton->setDefault(false);
    ui->skipButton->setFocusPolicy(Qt::ClickFocus);
    ui->skipButton->setEnabled(present);
    ui->skipButton->setVisible(present);
    ui->skipButton->setText(label);
    updateSize(false);
}

void ProgressDialog::on_skipButton_clicked([[maybe_unused]] bool checked)
{
    if (ui->skipButton->isEnabled() && m_task)
        m_task->abort();
}

void ProgressDialog::on_detailsButton_clicked()
{
    m_detailsExpanded = !m_detailsExpanded;
    ui->detailsContainer->setVisible(m_detailsExpanded);
    ui->detailsButton->setText(m_detailsExpanded ? tr("Hide Details ▲") : tr("More Details ▼"));
    updateSize(false);
}

ProgressDialog::~ProgressDialog()
{
    for (auto conn : this->m_taskConnections) {
        disconnect(conn);
    }
    delete ui;
}

void ProgressDialog::updateSize(bool recenterParent)
{
    adjustSize();

    if (recenterParent) {
        auto parent = this->parentWidget();
        QSize newSize = this->size();
        if (parent) {
            auto newX = std::max(0, parent->x() + ((parent->width() - newSize.width()) / 2));
            auto newY = std::max(0, parent->y() + ((parent->height() - newSize.height()) / 2));
            this->move(newX, newY);
        }
    }
}

int ProgressDialog::execWithTask(Task* task)
{
    this->m_task = task;

    if (!task) {
        qDebug() << "Programmer error: Progress dialog created with null task.";
        return QDialog::DialogCode::Accepted;
    }

    QDialog::DialogCode result{};
    if (handleImmediateResult(result)) {
        return result;
    }

    // Connect signals.
    this->m_taskConnections.push_back(connect(task, &Task::started, this, &ProgressDialog::onTaskStarted));
    this->m_taskConnections.push_back(connect(task, &Task::failed, this, &ProgressDialog::onTaskFailed));
    this->m_taskConnections.push_back(connect(task, &Task::succeeded, this, &ProgressDialog::onTaskSucceeded));
    this->m_taskConnections.push_back(connect(task, &Task::status, this, &ProgressDialog::changeStatus));
    this->m_taskConnections.push_back(connect(task, &Task::details, this, &ProgressDialog::changeStatus));
    this->m_taskConnections.push_back(connect(task, &Task::stepProgress, this, &ProgressDialog::changeStepProgress));
    this->m_taskConnections.push_back(connect(task, &Task::progress, this, &ProgressDialog::changeProgress));
    this->m_taskConnections.push_back(connect(task, &Task::aborted, this, &ProgressDialog::hide));
    this->m_taskConnections.push_back(connect(task, &Task::abortStatusChanged, ui->skipButton, &QPushButton::setEnabled));
    this->m_taskConnections.push_back(connect(task, &Task::abortButtonTextChanged, ui->skipButton, &QPushButton::setText));

    m_is_multi_step = task->isMultiStep();
    ui->taskProgressScrollArea->setVisible(m_is_multi_step);
    updateSize(true);

    // It's a good idea to start the task after we entered the dialog's event loop :^)
    if (!task->isRunning()) {
        QMetaObject::invokeMethod(task, &Task::start, Qt::QueuedConnection);
    } else {
        changeStatus(task->getStatus());
        changeProgress(task->getProgress(), task->getTotalProgress());
    }

    return QDialog::exec();
}

// TODO: only provide the unique_ptr overloads
int ProgressDialog::execWithTask(std::unique_ptr<Task>&& task)
{
    connect(this, &ProgressDialog::destroyed, task.get(), &Task::deleteLater);
    return execWithTask(task.release());
}
int ProgressDialog::execWithTask(std::unique_ptr<Task>& task)
{
    connect(this, &ProgressDialog::destroyed, task.get(), &Task::deleteLater);
    return execWithTask(task.release());
}

bool ProgressDialog::handleImmediateResult(QDialog::DialogCode& result)
{
    if (m_task->isFinished()) {
        if (m_task->wasSuccessful()) {
            result = QDialog::Accepted;
        } else {
            result = QDialog::Rejected;
        }
        return true;
    }
    return false;
}

Task* ProgressDialog::getTask()
{
    return m_task;
}

void ProgressDialog::onTaskStarted() {}

void ProgressDialog::onTaskFailed([[maybe_unused]] QString failure)
{
    reject();
    hide();
}

void ProgressDialog::onTaskSucceeded()
{
    accept();
    hide();
}

void ProgressDialog::changeStatus([[maybe_unused]] const QString& status)
{
    if (m_task) {
        ui->globalStatusLabel->setText(m_task->getStatus());
        QString details = m_task->getDetails();
        ui->globalStatusDetailsLabel->setText(details);
        ui->globalStatusDetailsLabel->setVisible(!details.isEmpty());
    }
}

void ProgressDialog::addTaskProgress(TaskStepProgress const& progress)
{
    SubTaskProgressBar* task_bar = new SubTaskProgressBar(this);
    taskProgress.insert(progress.uid, task_bar);
    ui->taskProgressLayout->addWidget(task_bar);
}

void ProgressDialog::changeStepProgress(TaskStepProgress const& task_progress)
{
    m_is_multi_step = true;
    ui->taskProgressScrollArea->setVisible(true);

    if (!taskProgress.contains(task_progress.uid))
        addTaskProgress(task_progress);
    auto task_bar = taskProgress.value(task_progress.uid);

    auto const [mapped_current, mapped_total] = map_int_zero_max<qint64>(task_progress.current, task_progress.total, 0);
    if (task_progress.total <= 0) {
        task_bar->setRange(0, 0);
    } else {
        task_bar->setRange(0, mapped_total);
    }

    task_bar->setValue(mapped_current);
    task_bar->setStatus(task_progress.status);
    task_bar->setDetails(task_progress.details);
    task_bar->setVisible(!task_progress.isDone());
}

void ProgressDialog::changeProgress(qint64 current, qint64 total)
{
    if (total <= 0) {
        ui->globalProgressBar->setRange(0, 0);
    } else {
        auto const [mapped_current, mapped_total] = map_int_zero_max<qint64>(std::max<qint64>(0, current), total, 0);
        ui->globalProgressBar->setRange(0, mapped_total);
        ui->globalProgressBar->setValue(mapped_current);
    }
}

void ProgressDialog::keyPressEvent(QKeyEvent* e)
{
    if (ui->skipButton->isVisible()) {
        if (e->key() == Qt::Key_Escape) {
            on_skipButton_clicked(true);
            return;
        } else if (e->key() == Qt::Key_Tab) {
            ui->skipButton->setFocusPolicy(Qt::StrongFocus);
            ui->skipButton->setFocus();
            ui->skipButton->setAutoDefault(true);
            ui->skipButton->setDefault(true);
            return;
        }
    }
    QDialog::keyPressEvent(e);
}

void ProgressDialog::closeEvent(QCloseEvent* e)
{
    if (m_task && m_task->isRunning()) {
        e->ignore();
    } else {
        QDialog::closeEvent(e);
    }
}
