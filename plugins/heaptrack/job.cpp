/*
    SPDX-FileCopyrightText: 2017 Anton Anikin <anton.anikin@htower.ru>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "job.h"

#include "debug.h"
#include "globalsettings.h"
#include "utils.h"

#include <execute/iexecuteplugin.h>
#include <interfaces/icore.h>
#include <interfaces/iuicontroller.h>
#include <util/environmentprofilelist.h>
#include <util/path.h>

#include <KLocalizedString>

#include <QFileInfo>
#include <QRegularExpression>

namespace {
[[nodiscard]] QString validEnvironmentProfileName(QString environmentProfileName)
{
    if (environmentProfileName.isEmpty()) {
        environmentProfileName = KDevelop::EnvironmentProfileList(KSharedConfig::openConfig()).defaultProfileName();
    }
    return environmentProfileName;
}
} // unnamed namespace

namespace Heaptrack
{

Job::Job(KDevelop::ILaunchConfiguration* launchConfig, IExecutePlugin* executePlugin)
{
    setCapabilities(Killable);

    Q_ASSERT(launchConfig);
    Q_ASSERT(executePlugin);

    QString errorString;
    const auto detectError = [&errorString, this](int errorCode) {
        if (errorString.isEmpty()) {
            return false;
        }
        setError(errorCode);
        setErrorText(errorString);
        return true;
    };

    const auto analyzedExecutable = executePlugin->executable(launchConfig, errorString).toLocalFile();
    if (detectError(-1)) {
        return;
    }
    QStringList analyzedExecutableArguments = executePlugin->arguments(launchConfig, errorString);
    if (detectError(-2)) {
        return;
    }

    setEnvironmentProfile(validEnvironmentProfileName(executePlugin->environmentProfileName(launchConfig)));

    const QFileInfo analyzedExecutableInfo(analyzedExecutable);

    QUrl workDir = executePlugin->workingDirectory(launchConfig);
    if (workDir.isEmpty() || !workDir.isValid()) {
        workDir = QUrl::fromLocalFile(analyzedExecutableInfo.absolutePath());
    }
    setWorkingDirectory(workDir);

    *this << KDevelop::Path(GlobalSettings::heaptrackExecutable()).toLocalFile();
    *this << analyzedExecutable;
    *this << analyzedExecutableArguments;

    setup(analyzedExecutableInfo.fileName());
}

Job::Job(long int pid)
{
    setCapabilities(Killable);

    const auto pidString = QString::number(pid);

    *this << KDevelop::Path(GlobalSettings::heaptrackExecutable()).toLocalFile();
    *this << QStringLiteral("-p");
    *this << pidString;

    // pass a QString as %1 to prevent treatment of the PID as amount and
    // inappropriate formatting according to locale rules (thousands separation)
    setup(i18nc("%1 - process ID", "PID: %1", pidString));
}

void Job::setup(const QString& targetName)
{
    setObjectName(i18n("Heaptrack Analysis (%1)", targetName));
    // shorten the output tab title to show more tabs without scrolling
    setTitle(i18nc("%1 - the name of the target of a Heaptrack analysis", "Heaptrack (%1)", targetName));

    setProperties(DisplayStdout);
    setProperties(DisplayStderr);
    setProperties(PostProcessOutput);

    setStandardToolView(KDevelop::IOutputView::DebugView);
    setBehaviours(KDevelop::IOutputView::AllowUserClose | KDevelop::IOutputView::AutoScroll);
    // Heaptrack analysis runs a native program and prints its output along with the output of Heaptrack itself
    setFilteringStrategy(KDevelop::OutputModel::NativeAppErrorFilter);

    KDevelop::ICore::self()->uiController()->registerStatus(this);
    connect(this, &Job::finished, this, [this]() {
        emit hideProgress(this);
    });
}

Job::~Job()
{
}

QString Job::statusName() const
{
    return objectName();
}

QString Job::resultsFile() const
{
    return m_resultsFile;
}

void Job::start()
{
    if (error() != NoError) {
        emitResult();
        return;
    }

    emit showProgress(this, 0, 0, 0);
    OutputExecuteJob::start();
}

void Job::postProcessStdout(const QStringList& lines)
{
    static const auto resultRegex =
        QRegularExpression(QStringLiteral("heaptrack output will be written to \\\"(.+)\\\""));

    if (m_resultsFile.isEmpty()) {
        QRegularExpressionMatch match;
        for (const QString & line : lines) {
            match = resultRegex.match(line);
            if (match.hasMatch()) {
                m_resultsFile = match.captured(1);
                break;
            }
        }
    }

    OutputExecuteJob::postProcessStdout(lines);
}

}

#include "moc_job.cpp"
