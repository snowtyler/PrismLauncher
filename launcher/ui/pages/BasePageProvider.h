/* Copyright 2013-2021 MultiMC Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <QIcon>
#include <functional>
#include "ui/pages/BasePage.h"

struct PageDescriptor {
    QString id;
    QString displayName;
    QIcon icon;
    QString helpPage;
    std::function<BasePage*()> creator;
};

class BasePageProvider {
   public:
    virtual QList<BasePage*> getPages() = 0;
    virtual QString dialogTitle() = 0;
    virtual bool supportsLazyPages() const { return false; }
    virtual const QList<PageDescriptor>& pageDescriptors() const
    {
        static const QList<PageDescriptor> empty;
        return empty;
    }
};

class GenericPageProvider : public BasePageProvider {
    using PageCreator = std::function<BasePage*()>;

   public:
    explicit GenericPageProvider(const QString& dialogTitle) : m_dialogTitle(dialogTitle) {}
    virtual ~GenericPageProvider() {}

    QList<BasePage*> getPages() override
    {
        QList<BasePage*> pages;
        for (auto& desc : m_descriptors) {
            pages.append(desc.creator());
        }
        for (auto& creator : m_creators) {
            pages.append(creator());
        }
        return pages;
    }
    QString dialogTitle() override { return m_dialogTitle; }

    bool supportsLazyPages() const override { return !m_descriptors.isEmpty() && m_creators.isEmpty(); }
    const QList<PageDescriptor>& pageDescriptors() const override { return m_descriptors; }

    void setDialogTitle(const QString& title) { m_dialogTitle = title; }
    void addPageCreator(PageCreator page) { m_creators.append(page); }

    template <typename PageClass>
    void addPage(const QString& id, const QString& displayName, const QIcon& icon, const QString& helpPage = {})
    {
        m_descriptors.append({ id, displayName, icon, helpPage, []() { return new PageClass(); } });
    }

   private:
    QList<PageDescriptor> m_descriptors;
    QList<PageCreator> m_creators;
    QString m_dialogTitle;
};
