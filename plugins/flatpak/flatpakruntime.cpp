/*
    SPDX-FileCopyrightText: 2017 Aleix Pol Gonzalez <aleixpol@kde.org>

    SPDX-License-Identifier: LGPL-2.0-only
*/

#include "flatpakruntime.h"
#include "debug_flatpak.h"

#include <util/executecompositejob.h>
#include <outputview/outputexecutejob.h>
#include <interfaces/iruncontroller.h>
#include <interfaces/icore.h>

#include <KLocalizedString>
#include <KProcess>
#include <KActionCollection>
#include <QProcess>
#include <QTemporaryDir>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>

using namespace KDevelop;

namespace {

template <typename T, typename Q, typename W>
static T kTransform(const Q& list, W func)
{
    T ret;
    ret.reserve(list.size());
    for (auto it = list.constBegin(), itEnd = list.constEnd(); it!=itEnd; ++it)
        ret += func(*it);
    return ret;
}

class FlatpakJob : public OutputExecuteJob
{
public:
    /**
     * Create a Flatpak output job.
     *
     * @param jobName the name of the job
     * @param runOutputTabTitle the title of the job's output tab in the standard Run output tool view;
     *        if empty, the job uses the nonstandard "Flatpak" output tool view and the job title equals the job name
     * @param parent the job's parent
     */
    explicit FlatpakJob(const QString& jobName, const QString& runOutputTabTitle = {}, QObject* parent = nullptr)
        : OutputExecuteJob(parent)
    {
        setCapabilities(Killable);

        if (runOutputTabTitle.isEmpty()) {
            // Create a custom output tool view instead of reusing the standard Build tool view
            // because there are many separate low-level Flatpak build jobs that run one after another
            // and would bury likely more interesting build outputs in the long output history.
            setToolViewId(QStringLiteral("Flatpak"));
            setToolTitle(i18nc("@title:window", "Flatpak"));
            // TODO: set a Flatpak icon as the tool icon
            setViewType(IOutputView::HistoryView);
            // The select and focus buttons are useless because the job has no output filtering strategy.
            // The manual filtering of messages can be useful.
            setOptions(IOutputView::AddFilterAction);

            setJobName(jobName);
        } else {
            setStandardToolView(IOutputView::RunView);
            setFilteringStrategy(OutputModel::NativeAppErrorFilter);

            setObjectName(jobName);
            setTitle(runOutputTabTitle);
        }

        // Automatically scroll the view like most other output views do.
        setBehaviours(IOutputView::AllowUserClose | IOutputView::AutoScroll);
        setProperties(DisplayStdout | DisplayStderr);
    }
};

OutputExecuteJob* createExecuteJob(const QStringList& program, const QString& jobName,
                                   const QString& runOutputTabTitle = {})
{
    auto* const process = new FlatpakJob(jobName, runOutputTabTitle);
    process->setExecuteOnHost(true);
    *process << program;
    return process;
}

} // unnamed namespace

KJob* FlatpakRuntime::createBuildDirectory(const KDevelop::Path &buildDirectory, const KDevelop::Path &file, const QString &arch)
{
    const auto sourceDirectory = file.parent();

    auto identifyingName = file.lastPathSegment();
    if (identifyingName == kdeFlatpakManifestFileName) {
        // The file name is a standard name that does not identify the application.
        // Display the hopefully more useful name of the containing directory.
        identifyingName = sourceDirectory.lastPathSegment();
    } else {
        constexpr QLatin1String standardExtension(".json");
        if (identifyingName.endsWith(standardExtension)) {
            identifyingName.chop(standardExtension.size());
        }
    }

    auto* const job = createExecuteJob({QStringLiteral("flatpak-builder"), QLatin1String("--arch=") + arch,
                                        QStringLiteral("--build-only"), QStringLiteral("--force-clean"),
                                        QStringLiteral("--ccache"), buildDirectory.toLocalFile(), file.toLocalFile()},
                                       i18nc("%1 - application ID or name", "Flatpak Create %1", identifyingName));
    job->setWorkingDirectory(sourceDirectory.toUrl());
    return job;
}

FlatpakRuntime::FlatpakRuntime(const KDevelop::Path &buildDirectory, const KDevelop::Path &file, const QString &arch)
    : KDevelop::IRuntime()
    , m_file(file)
    , m_buildDirectory(buildDirectory)
    , m_arch(arch)
{
    refreshJson();
}

FlatpakRuntime::~FlatpakRuntime()
{
}

void FlatpakRuntime::refreshJson()
{
    const auto doc = config();
    const QString sdkName = doc[QLatin1String("sdk")].toString();
    const QString runtimeVersion = doc.value(QLatin1String("runtime-version")).toString();
    const QString usedRuntime = sdkName + QLatin1Char('/') + m_arch + QLatin1Char('/') + runtimeVersion;

    //First check if local user has flatpak runtime before checking system runtimes.
    m_sdkPath = KDevelop::Path(QDir::homePath() + QLatin1String("/.local/share/flatpak/runtime/") + usedRuntime + QLatin1String("/active/files"));
    if(!QFile::exists(m_sdkPath.toLocalFile())) {
        m_sdkPath = KDevelop::Path(QLatin1String("/var/lib/flatpak/runtime/") + usedRuntime + QLatin1String("/active/files"));
    }
    qCDebug(FLATPAK) << "flatpak runtime path..." << name() << m_sdkPath;
    Q_ASSERT(QFile::exists(m_sdkPath.toLocalFile()));

    m_finishArgs = kTransform<QStringList>(doc[QLatin1String("finish-args")].toArray(), [](const QJsonValue& val){ return val.toString(); });
}

void FlatpakRuntime::setEnabled(bool /*enable*/)
{
}

//Take any environment variables specified in process to pass through to flatpak.
static QStringList envVarsForProcess(const QProcess* process)
{
    QStringList env_args;
    const QStringList env_vars = process->processEnvironment().toStringList();
    const QStringList system = QProcessEnvironment::systemEnvironment().toStringList();
    for (const QString& env_var : env_vars) {
        if (!system.contains(
                env_var)) { // Filter out the ones inherited from the system, let flatpak decide what to do there
            env_args << QLatin1String("--env=") + env_var;
        }
    }
    return env_args;
}

void FlatpakRuntime::startProcess(QProcess* process) const
{
    const QStringList args = envVarsForProcess(process)
        << QStringList{QStringLiteral("--run"), m_buildDirectory.toLocalFile(), m_file.toLocalFile(),
                       process->program()}
        << process->arguments();
    process->setProgram(QStringLiteral("flatpak-builder"));
    process->setArguments(args);

    qCDebug(FLATPAK) << "starting qprocess" << process->program() << process->arguments();
    process->start();
}

void FlatpakRuntime::startProcess(KProcess* process) const
{
    process->setProgram(QStringList{QStringLiteral("flatpak-builder")}
                        << envVarsForProcess(process)
                        << QStringList{QStringLiteral("--run"), m_buildDirectory.toLocalFile(), m_file.toLocalFile()}
                        << process->program());

    qCDebug(FLATPAK) << "starting kprocess" << process->program().join(QLatin1Char(' '));
    process->start();
}

KJob* FlatpakRuntime::rebuild()
{
    QDir(m_buildDirectory.toLocalFile()).removeRecursively();
    auto job = createBuildDirectory(m_buildDirectory, m_file, m_arch);
    refreshJson();
    return job;
}

auto FlatpakRuntime::exportBundle(const QString& path) const -> ExportBundle
{
    const auto doc = config();

    auto* dir = new QTemporaryDir(QDir::tempPath()+QLatin1String("/flatpak-tmp-repo"));
    if (!dir->isValid() || doc.isEmpty()) {
        qCWarning(FLATPAK) << "Couldn't export:" << path << dir->isValid() << dir->path() << doc.isEmpty();
        return {};
    }

    auto name = doc[QLatin1String("id")].toString();
    QStringList args = m_finishArgs;
    if (doc.contains(QLatin1String("command")))
        args << QLatin1String("--command=")+doc[QLatin1String("command")].toString();

    auto* const buildFinishJob = createExecuteJob(
        QStringList{QStringLiteral("flatpak"), QStringLiteral("build-finish"), m_buildDirectory.toLocalFile()} << args,
        i18nc("%1 - application ID", "Flatpak Finalize %1", name));
    buildFinishJob->setCheckExitCode(false);

    QList<KJob*> jobs{buildFinishJob,
                      createExecuteJob({QStringLiteral("flatpak"), QStringLiteral("build-export"),
                                        QLatin1String("--arch=") + m_arch, dir->path(), m_buildDirectory.toLocalFile()},
                                       i18nc("%1 - application ID", "Flatpak Export %1", name)),
                      createExecuteJob({QStringLiteral("flatpak"), QStringLiteral("build-bundle"),
                                        QLatin1String("--arch=") + m_arch, dir->path(), path, name},
                                       i18nc("%1 - application ID", "Flatpak Bundle %1", name))};
    connect(jobs.last(), &QObject::destroyed, jobs.last(), [dir]() { delete dir; });

    return {std::move(jobs), std::move(name)};
}

QString FlatpakRuntime::name() const
{
    return QStringLiteral("%1 - %2").arg(m_arch, config()[u"id"].toString());
}

KJob * FlatpakRuntime::executeOnDevice(const QString& host, const QString &path) const
{
    const QString name = config()[QLatin1String("id")].toString();
    const QString destPath = QStringLiteral("/tmp/kdevelop-test-app.flatpak");
    const QString replicatePath = QStringLiteral("/tmp/replicate.sh");
    const QString localReplicatePath = QStandardPaths::locate(QStandardPaths::AppDataLocation, QStringLiteral("kdevflatpak/replicate.sh"));
    const auto runJobName = i18nc("%1 - application ID, %2 - device host name", "Flatpak Run %1 on %2", name, host);

    const auto jobs = exportBundle(path).jobs
        << QList<KJob*>{
               createExecuteJob({QStringLiteral("scp"), path, host + QLatin1Char(':') + destPath},
                                i18nc("%1 - device host name", "Transfer .flatpak to %1", host)),
               createExecuteJob({QStringLiteral("scp"), localReplicatePath, host + QLatin1Char(':') + replicatePath},
                                i18nc("%1 - device host name", "Transfer replicate.sh to %1", host)),
               createExecuteJob(
                   {QStringLiteral("ssh"), host, QStringLiteral("flatpak"), QStringLiteral("install"),
                    QStringLiteral("--user"), QStringLiteral("--bundle"), QStringLiteral("-y"), destPath},
                   i18nc("%1 - application ID, %2 - device host name", "Flatpak Install %1 to %2", name, host)),
               createExecuteJob({QStringLiteral("ssh"), host, QStringLiteral("bash"), replicatePath,
                                 QStringLiteral("plasmashell"), QStringLiteral("flatpak"), QStringLiteral("run"), name},
                                runJobName, name)};

    auto* const compositeJob = new ExecuteCompositeJob(parent(), jobs);
    compositeJob->setObjectName(runJobName);
    return compositeJob;
}

QJsonObject FlatpakRuntime::config(const KDevelop::Path& path)
{
    QFile f(path.toLocalFile());
    if (!f.open(QIODevice::ReadOnly)) {
        qCWarning(FLATPAK) << "couldn't open" << path;
        return {};
    }

    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(f.readAll(), &error);
    if (error.error) {
        qCWarning(FLATPAK) << "couldn't parse" << path << error.errorString();
        return {};
    }

    return doc.object();
}

QJsonObject FlatpakRuntime::config() const
{
    return config(m_file);
}

Path FlatpakRuntime::pathInHost(const KDevelop::Path& runtimePath) const
{
    KDevelop::Path ret = runtimePath;
    if (!runtimePath.isLocalFile()) {
        return ret;
    }

    const auto prefix = runtimePath.segments().at(0);
    if (prefix == QLatin1String("usr")) {
        const auto relpath = KDevelop::Path(QStringLiteral("/usr")).relativePath(runtimePath);
        ret = Path(m_sdkPath, relpath);
    } else if (prefix == QLatin1String("app")) {
        const auto relpath = KDevelop::Path(QStringLiteral("/app")).relativePath(runtimePath);
        ret = Path(m_buildDirectory, QLatin1String("/active/files/") + relpath);
    }

    qCDebug(FLATPAK) << "path in host" << runtimePath << ret;
    return ret;
}

Path FlatpakRuntime::pathInRuntime(const KDevelop::Path& localPath) const
{
    KDevelop::Path ret = localPath;
    if (m_sdkPath.isParentOf(localPath)) {
        const auto relpath = m_sdkPath.relativePath(localPath);
        ret = Path(Path(QStringLiteral("/usr")), relpath);
    } else {
        const Path bdfiles(m_buildDirectory, QStringLiteral("/active/files"));
        if (bdfiles.isParentOf(localPath)) {
            const auto relpath = bdfiles.relativePath(localPath);
            ret = Path(Path(QStringLiteral("/app")), relpath);
        }
    }

    qCDebug(FLATPAK) << "path in runtime" << localPath << ret;
    return ret;
}

QString FlatpakRuntime::findExecutable(const QString& executableName) const
{
    QStringList rtPaths;

    auto envPaths = getenv(QByteArrayLiteral("PATH")).split(':');
    std::transform(envPaths.begin(), envPaths.end(), std::back_inserter(rtPaths),
                    [this](QByteArray p) {
                        return pathInHost(Path(QString::fromLocal8Bit(p))).toLocalFile();
                    });

    return QStandardPaths::findExecutable(executableName, rtPaths);
}

QByteArray FlatpakRuntime::getenv(const QByteArray& varname) const
{
    if (varname == "KDEV_DEFAULT_INSTALL_PREFIX")
        return "/app";
    return qgetenv(varname.constData());
}

KDevelop::Path FlatpakRuntime::buildPath() const
{
    auto file = m_file;
    file.setLastPathSegment(QStringLiteral(".flatpak-builder"));
    file.addPath(QStringLiteral("kdevelop"));
    return file;
}

#include "moc_flatpakruntime.cpp"
