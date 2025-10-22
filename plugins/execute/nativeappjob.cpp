/*
    SPDX-FileCopyrightText: 2009 Andreas Pakulat <apaku@gmx.de>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "nativeappjob.h"

#include <QAbstractButton>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QCheckBox>

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <interfaces/ilaunchconfiguration.h>
#include <interfaces/iruncontroller.h>
#include <outputview/outputmodel.h>
#include <util/environmentprofilelist.h>

#include <interfaces/icore.h>
#include <interfaces/iplugincontroller.h>
#include <project/projectmodel.h>

#include "executeplugin.h"
#include "debug.h"

using namespace KDevelop;

namespace {
[[nodiscard]] QString validEnvironmentProfileName(const QString& launchConfigurationName,
                                                  QString environmentProfileName)
{
    if (environmentProfileName.isEmpty()) {
        qCWarning(PLUGIN_EXECUTE).noquote() << i18n(
            "No environment profile specified, looks like a broken configuration, please check run configuration '%1'. "
            "Using default environment profile.",
            launchConfigurationName);
        environmentProfileName = EnvironmentProfileList(KSharedConfig::openConfig()).defaultProfileName();
    }
    return environmentProfileName;
}
} // unnamed namespace

NativeAppJob::NativeAppJob(QObject* parent, KDevelop::ILaunchConfiguration* cfg)
    : KDevelop::OutputExecuteJob( parent )
    , m_name(cfg->name())
{
    setCapabilities(Killable);

    auto* iface = KDevelop::ICore::self()->pluginController()->pluginForExtension(QStringLiteral("org.kdevelop.IExecutePlugin"), QStringLiteral("kdevexecute"))->extension<IExecutePlugin>();
    Q_ASSERT(iface);

    QString err;
    const auto detectError = [&err, this](int errorCode) {
        if (err.isEmpty()) {
            return false;
        }
        setError(errorCode);
        setErrorText(err);
        return true;
    };

    const auto executable = iface->executable(cfg, err).toLocalFile();
    if (detectError(-1)) {
        return;
    }
    QStringList arguments = iface->arguments( cfg, err );
    if (detectError(-2)) {
        return;
    }

    QStringList terminal;
    if (iface->useTerminal(cfg)) {
        terminal = iface->terminal(cfg, err);
        if (detectError(-3)) {
            return;
        }
    }

    const auto launchConfigurationName = m_name;
    {
        const auto cfgGroup = cfg->config();
        if (cfgGroup.readEntry(ExecutePlugin::isExecutableEntry, false)) {
            m_name = cfgGroup.readEntry(ExecutePlugin::executableEntry, launchConfigurationName)
                         .section(QLatin1Char('/'), -1);
        }
        if (!cfgGroup.readEntry(ExecutePlugin::configuredByCTest, false)) {
            m_killBeforeExecutingAgain = cfgGroup.readEntry<int>(ExecutePlugin::killBeforeExecutingAgain, askIfRunning);
        }
    }

    setEnvironmentProfile(validEnvironmentProfileName(launchConfigurationName, iface->environmentProfileName(cfg)));

    setStandardToolView(KDevelop::IOutputView::RunView);
    setBehaviours(KDevelop::IOutputView::AllowUserClose | KDevelop::IOutputView::AutoScroll);
    setFilteringStrategy(OutputModel::NativeAppErrorFilter);
    setProperties(DisplayStdout | DisplayStderr);

    // Now setup the process parameters

    QUrl wc = iface->workingDirectory( cfg );
    if( !wc.isValid() || wc.isEmpty() ) {
        wc = QUrl::fromLocalFile(QFileInfo{executable}.absolutePath());
    }
    setWorkingDirectory( wc );

    qCDebug(PLUGIN_EXECUTE) << "setting app:" << executable << arguments;

    if (!terminal.empty()) {
        *this << std::move(terminal);
    }
    *this << executable;
    *this << arguments;

    setJobName(m_name);
}

NativeAppJob* findNativeJob(KJob* j)
{
    auto* job = qobject_cast<NativeAppJob*>(j);
    if (!job) {
        const QList<NativeAppJob*> jobs = j->findChildren<NativeAppJob*>();
        if (!jobs.isEmpty())
            job = jobs.first();
    }
    return job;
}

void NativeAppJob::start()
{
    if (error() != NoError) {
        emitResult();
        return;
    }

    QVector<QPointer<NativeAppJob> > currentJobs;
    // collect running instances of the same type
    const auto& allCurrentJobs = ICore::self()->runController()->currentJobs();
    for (auto j : allCurrentJobs) {
        NativeAppJob* njob = findNativeJob(j);
        if (njob && njob != this && njob->m_name == m_name)
            currentJobs << njob;
    }

    if (!currentJobs.isEmpty()) {
        int oldJobAction = m_killBeforeExecutingAgain;
        if (oldJobAction == askIfRunning) {
            QMessageBox msgBox(QMessageBox::Question,
                        i18nc("@title:window", "Job Already Running"),
                        i18n("'%1' is already being executed.", m_name),
                        startAnother | killAllInstances | QMessageBox::Cancel /* aka askIfRunning */);
            msgBox.button(killAllInstances)->setText(i18nc("@action:button", "Kill All Instances"));
            msgBox.button(startAnother)->setText(i18nc("@action:button", "Start Another"));
            msgBox.setDefaultButton(QMessageBox::Cancel);

            QCheckBox* remember = new QCheckBox(i18nc("@option:check", "Remember choice"));
            msgBox.setCheckBox(remember);

            oldJobAction = msgBox.exec();
            if (remember->isChecked() && oldJobAction != QMessageBox::Cancel) {
                Q_EMIT killBeforeExecutingAgainChanged(oldJobAction);
            }
        }

        switch (oldJobAction) {
            case startAnother:
                break;
            case killAllInstances:
                for (auto & job : currentJobs) {
                    if (job)
                        job->kill();
                }
                break;
            default: // cancel starting a new job
                kill();
                return;
        }
    }

    OutputExecuteJob::start();
}

#include "moc_nativeappjob.cpp"
