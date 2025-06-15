/*
    SPDX-FileCopyrightText: 2006 Alexander Dymo <adymo@kdevelop.org>
    SPDX-FileCopyrightText: 2007 Andreas Pakulat <apaku@gmx.de>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "kdevfilemanagerplugin.h"

#include <KLocalizedString>
#include <KPluginFactory>

#include <interfaces/icore.h>
#include <interfaces/iuicontroller.h>

#include "filemanager.h"

K_PLUGIN_FACTORY_WITH_JSON(KDevFileManagerFactory, "kdevfilemanager.json", registerPlugin<KDevFileManagerPlugin>();)

class KDevFileManagerViewFactory: public KDevelop::IToolViewFactory{
public:
    KDevFileManagerViewFactory() = default;

    QWidget* create(QWidget *parent = nullptr) override
    {
        return new FileManager(parent);
    }

    QList<QAction*> toolBarActions( QWidget* w ) const override
    {
        auto* m = qobject_cast<FileManager*>(w);
        if( m )
            return m->toolBarActions();
        return KDevelop::IToolViewFactory::toolBarActions( w );
    }

    Qt::DockWidgetArea defaultPosition() const override
    {
        return Qt::LeftDockWidgetArea;
    }

    QString id() const override
    {
        return QStringLiteral("org.kdevelop.FileManagerView");
    }
};

KDevFileManagerPlugin::KDevFileManagerPlugin(QObject* parent, const KPluginMetaData& metaData,
                                             const QVariantList& /*args*/)
    : KDevelop::IPlugin(QStringLiteral("kdevfilemanager"), parent, metaData)
{
    setXMLFile(QStringLiteral("kdevfilemanager.rc"));

    m_factory = new KDevFileManagerViewFactory;
    core()->uiController()->addToolView(i18nc("@title:window", "File System"), m_factory);
}

KDevFileManagerPlugin::~KDevFileManagerPlugin()
{
}

void KDevFileManagerPlugin::unload()
{
    core()->uiController()->removeToolView(m_factory);
}

#include "kdevfilemanagerplugin.moc"
#include "moc_kdevfilemanagerplugin.cpp"
