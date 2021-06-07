#include "greeterworkek.h"
#include "src/session-widgets/sessionbasemodel.h"
#include "src/session-widgets/userinfo.h"
#include "src/global_util/keyboardmonitor.h"
#include "src/session-widgets/framedatabind.h"

#include <libintl.h>
#include <DSysInfo>

#define LOCKSERVICE_PATH "/com/deepin/dde/LockService"
#define LOCKSERVICE_NAME "com.deepin.dde.LockService"

using namespace Auth;
DCORE_USE_NAMESPACE

const QString AuthenticateService("com.deepin.daemon.Authenticate");

void FileIOThread::run() {
    int fd;
    char *filename = "/dev/input/event0";

    if ((fd = open(filename, O_RDONLY)) < 0) {
        if (errno == EACCES && getuid() != 0) {
            return;
        }
    }

    while (1) {
        if (1 == print_events(fd))
            return;
    }
}

int FileIOThread::print_events(int fd) {
    struct input_event ev[64];
    int i, rd;
    fd_set rdfs;

    FD_ZERO(&rdfs);
    FD_SET(fd, &rdfs);

    rd = read(fd, ev, sizeof(ev));

    if (rd < (int) sizeof(struct input_event)) {
        close(fd);
        return 1;
    }

    for (i = 0; i < rd / sizeof(struct input_event); i++) {
        if (1 == ev[i].value) {
            Q_EMIT runShutDownProcess();
        }
    }

    ioctl(fd, EVIOCGRAB, (void*)0);
    return EXIT_SUCCESS;
}

class UserNumlockSettings
{
public:
    UserNumlockSettings(const QString &username)
        : m_username(username)
        , m_settings(QSettings::UserScope, "deepin", "greeter")
    {
    }

    bool get(const bool defaultValue) { return m_settings.value(m_username, defaultValue).toBool(); }
    void set(const bool value) { m_settings.setValue(m_username, value); }

private:
    QString m_username;
    QSettings m_settings;
};

GreeterWorkek::GreeterWorkek(SessionBaseModel *const model, QObject *parent)
    : AuthInterface(model, parent)
    , m_greeter(new QLightDM::Greeter(this))
    , m_lockInter(new LockService(LOCKSERVICE_NAME, LOCKSERVICE_PATH, QDBusConnection::systemBus(), this))
    , m_AuthenticateInter(new Authenticate(AuthenticateService,
                                         "/com/deepin/daemon/Authenticate",
                                         QDBusConnection::systemBus(), this))
    , m_isThumbAuth(false)
    , m_authenticating(false)
    , m_showAuthResult(false)
    , m_firstTimeLoginForOneKey(true)
    , m_authSuccess(false)
    , m_password(QString())
{
    if (m_AuthenticateInter->isValid()) {
        m_AuthenticateInter->setTimeout(300);
    }

    if (!m_greeter->connectSync()) {
        qWarning() << "greeter connect fail !!!";
    }

    connect(m_greeter, &QLightDM::Greeter::showPrompt, this, &GreeterWorkek::prompt);
    connect(m_greeter, &QLightDM::Greeter::showMessage, this, &GreeterWorkek::message);
    connect(m_greeter, &QLightDM::Greeter::authenticationComplete, this, &GreeterWorkek::authenticationComplete);

    connect(model, &SessionBaseModel::onPowerActionChanged, this, [ = ](SessionBaseModel::PowerAction poweraction) {
        switch (poweraction) {
        case SessionBaseModel::PowerAction::RequireShutdown:
            m_login1Inter->PowerOff(true);
            break;
        case SessionBaseModel::PowerAction::RequireRestart:
            m_login1Inter->Reboot(true);
            break;
        case SessionBaseModel::PowerAction::RequireSuspend:
            m_login1Inter->Suspend(true);
            break;
        case SessionBaseModel::PowerAction::RequireHibernate:
            m_login1Inter->Hibernate(true);
            break;
        default:
            break;
        }

        model->setPowerAction(SessionBaseModel::PowerAction::None);
    });

    connect(model, &SessionBaseModel::lockChanged, this, [ = ](bool lock) {
        if (!lock) {
            m_password.clear();
            qDebug() << "Request Auth_SessionBaseModel::lockChanged --  3min lock finish" << m_model->currentUser()->name();
            resetLightdmAuth(m_model->currentUser(), 100, false);
        }
    });

    connect(KeyboardMonitor::instance(), &KeyboardMonitor::numlockStatusChanged, this, [ = ](bool on) {
        saveNumlockStatus(model->currentUser(), on);
    });

    connect(model, &SessionBaseModel::currentUserChanged, this, &GreeterWorkek::recoveryUserKBState);
    connect(m_lockInter, &LockService::UserChanged, this, &GreeterWorkek::onCurrentUserChanged);
    connect(m_lockInter, &LockService::IsDisablePasswdChanged, model, &SessionBaseModel::setIsDisablePasswordEdit);

    connect(m_login1Inter, &DBusLogin1Manager::SessionRemoved, this, [ = ] {
        const QString& user = m_lockInter->CurrentUser();
        const QJsonObject obj = QJsonDocument::fromJson(user.toUtf8()).object();
        auto user_ptr = m_model->findUserByUid(static_cast<uint>(obj["Uid"].toInt()));

        m_model->setCurrentUser(user_ptr);
        userAuthForLightdm(user_ptr);
    });

    // 待机，休眠都会有PrepareForSleep，统一处理
    connect(m_login1Inter, &DBusLogin1Manager::PrepareForSleep, this, [ = ](bool active) {
        qDebug() << "set dpms: " << (!active ? "on" : "off");
        screenSwitchByWldpms(!active);
    });

    const QString &switchUserButtonValue { valueByQSettings<QString>("Lock", "showSwitchUserButton", "ondemand") };
    m_model->setAlwaysShowUserSwitchButton(switchUserButtonValue == "always");
    m_model->setAllowShowUserSwitchButton(switchUserButtonValue == "ondemand");

    m_model->setIsDisablePasswordEdit(m_lockInter->isDisablePasswd());

    {
        initDBus();
        initData();

        if (QFile::exists("/etc/deepin/no_suspend"))
            m_model->setCanSleep(false);

        checkDBusServer(m_accountsInter->isValid());
        oneKeyLogin();
    }

    // 平安科技1022专业版需要支持用户手动输入用户名和密码去请求认证
    if (DSysInfo::deepinType() == DSysInfo::DeepinProfessional){
        std::shared_ptr<User> user = std::make_shared<ADDomainUser>(INT_MAX);
        static_cast<ADDomainUser *>(user.get())->setUserDisplayName("...");
        m_model->userAdd(user);
        m_model->setCurrentUser(user);
    }

    if (DSysInfo::deepinType() == DSysInfo::DeepinServer || valueByQSettings<bool>("", "loginPromptInput", false)) {
        std::shared_ptr<User> user = std::make_shared<ADDomainUser>(INT_MAX);
        static_cast<ADDomainUser *>(user.get())->setUserDisplayName("...");
        static_cast<ADDomainUser *>(user.get())->setIsServerUser(true);
        m_model->setIsServerModel(true);
        m_model->userAdd(user);
        m_model->setCurrentUser(user);
    } else {
        connect(m_login1Inter, &DBusLogin1Manager::SessionRemoved, this, [ = ] {
            const QString& user = m_lockInter->CurrentUser();
            const QJsonObject obj = QJsonDocument::fromJson(user.toUtf8()).object();
            auto user_ptr = m_model->findUserByUid(static_cast<uint>(obj["Uid"].toInt()));

            m_model->setCurrentUser(user_ptr);
            userAuthForLightdm(user_ptr);
        });
    }

    m_monitorHisiThread = new FileIOThread();
    connect(m_monitorHisiThread, &FileIOThread::runShutDownProcess, this, [=] (){
        if (!m_login1Inter->preparingForSleep()) {
            model->setCurrentModeState(SessionBaseModel::ModeStatus::PowerMode);
        }
    });
    m_monitorHisiThread->start();
}

void GreeterWorkek::switchToUser(std::shared_ptr<User> user)
{
    qDebug() << "switch user from" << m_model->currentUser()->name() << " to "
             << user->name();

    Auth::AuthInterface::switchToUser(user);
    // clear old password
    m_password.clear();
    m_authenticating = false;

    // just switch user
    if (user->isLogin()) {
        // switch to user Xorg
        QProcess::startDetached("dde-switchtogreeter", QStringList() << user->name());
    }

    QJsonObject json;
    json["Uid"] = static_cast<int>(user->uid());
    json["Type"] = user->type();
    m_lockInter->SwitchToUser(QString(QJsonDocument(json).toJson(QJsonDocument::Compact))).waitForFinished();
}

void GreeterWorkek::authUser(const QString &password)
{
    if (m_authSuccess) {
        qDebug() << "shouldn't auth user after success auth: " << m_greeter->authenticationUser();
        return;
    }

    if (m_authenticating) return;

    m_authenticating = true;

    // auth interface
    std::shared_ptr<User> user = m_model->currentUser();

    m_password = password;

    qDebug() << "greeter authenticate user: " << m_greeter->authenticationUser() << " current user: " << user->name();
    if (m_greeter->authenticationUser() != user->name()) {
        qDebug() << "Request Auth_GreeterWorkek::authUser --  if authUser not currentUser" << user->name();
        resetLightdmAuth(user, 100, false);
    }
    else {
        if (m_greeter->inAuthentication()) {
            m_greeter->respond(password);
        }
        else {
            qDebug() << "Request Auth_GreeterWorkek::authUser -- auth finish once" << user->name();
            m_greeter->authenticate(user->name());
        }
    }
}

void GreeterWorkek::onUserAdded(const QString &user)
{
    std::shared_ptr<User> user_ptr(new NativeUser(user));

    user_ptr->setisLogind(isLogined(user_ptr->uid()));

    if (m_model->currentUser().get() == nullptr) {
        if (m_model->userList().isEmpty() || m_model->userList().first()->type() == User::ADDomain) {
            m_model->setCurrentUser(user_ptr);

            if (m_model->currentType() == SessionBaseModel::AuthType::LightdmType) {
                qDebug() << "Request Auth_GreeterWorkek::onUserAdded -- User added" << user_ptr->name();
                userAuthForLightdm(user_ptr);
            }
        }
    }

    if (user_ptr->uid() == m_lastLogoutUid) {
        m_model->setLastLogoutUser(user_ptr);
    }

    m_model->userAdd(user_ptr);
}

void GreeterWorkek::checkDBusServer(bool isvalid)
{
    if (isvalid) {
        onUserListChanged(m_accountsInter->userList());
    } else {
        // FIXME: 我不希望这样做，但是QThread::msleep会导致无限递归
        QTimer::singleShot(300, this, [ = ] {
            qWarning() << "com.deepin.daemon.Accounts is not start, rechecking!";
            checkDBusServer(m_accountsInter->isValid());
        });
    }
}

void GreeterWorkek::oneKeyLogin()
{
    if (!m_firstTimeLoginForOneKey) {
        callAuthForLightdm(m_lockInter->CurrentUser());
        return;
    }

    // 多用户一键登陆
//    auto user_firstlogin = m_AuthenticateInter->PreOneKeyLogin(AuthFlag::Fingerprint);
//    user_firstlogin.waitForFinished();
//    qDebug() << "zl: GreeterWorkek::onFirstTimeLogin -- FirstTime Login User Name is : " << user_firstlogin;

    // 以上通过Qt库调用dbus服务未能得到正确的返回结果，保留以示区别
    QString user_firstlogin;
    QProcess* process = new QProcess(this);
    process->start(QString("qdbus --system com.deepin.daemon.Authenticate /com/deepin/daemon/Authenticate com.deepin.daemon.Authenticate.PreOneKeyLogin %1").arg(AuthFlag::Fingerprint));
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [ & ] {
        QString data = QString(process->readAllStandardOutput());
        user_firstlogin = data.simplified();
    });
    process->waitForFinished();

    auto user_ptr = m_model->findUserByName(user_firstlogin);
    if (user_ptr.get() != nullptr) {
        qDebug() << __func__ << __LINE__ << "suo: oneKeyLogin";
        switchToUser(user_ptr);
        m_model->setCurrentUser(user_ptr);
        userAuthForLightdm(user_ptr);
        m_showAuthResult = true;
    } else {
        qDebug() << __func__ << __LINE__ << "suo: Log out";
        m_firstTimeLoginForOneKey = false;
        callAuthForLightdm(m_lockInter->CurrentUser());
    }
}

void GreeterWorkek::onCurrentUserChanged(const QString &user)
{
    qDebug() << "zl: current user " << user;
    const QJsonObject obj = QJsonDocument::fromJson(user.toUtf8()).object();
    m_currentUserUid = static_cast<uint>(obj["Uid"].toInt());

    //控制多用户一键登录时不会继续执行
    if (m_firstTimeLoginForOneKey) {
        return;
    }

    for (std::shared_ptr<User> user_ptr : m_model->userList()) {
        if (!user_ptr->isLogin() && user_ptr->uid() == m_currentUserUid) {
            m_model->setCurrentUser(user_ptr);
            qDebug() << "Request Auth_GreeterWorkek::onCurrentUserChanged -- currentUser changed to" << user_ptr->name();
            if (!user_ptr->isNoPasswdGrp()) {
                QTimer::singleShot(100, this, [ = ] {
                    m_greeter->authenticate(user_ptr->name());
                });
            }
            break;
        }
    }
}

void GreeterWorkek::userAuthForLightdm(std::shared_ptr<User> user)
{
    qDebug() << "suo" << __func__ << __LINE__ << "user" << user->name();
    if (!user->isNoPasswdGrp()) {
        //后端需要大约600ms时间去释放指纹设备
        resetLightdmAuth(user, 100, true);
    }
}

void GreeterWorkek::prompt(QString text, QLightDM::Greeter::PromptType type)
{
    // Don't show password prompt from standard pam modules since
    // we'll provide our own prompt or just not.
    const QString msg = text.simplified() == "Password:" ? "" : text;

    switch (type) {
    case QLightDM::Greeter::PromptTypeSecret:
        if (m_isThumbAuth || m_password.isEmpty()) break;

        if (msg.isEmpty()) {
            m_greeter->respond(m_password);
        } else {
            emit m_model->authFaildMessage(msg);
        }
        break;
    case QLightDM::Greeter::PromptTypeQuestion:
        emit m_model->authTipsMessage(text);
        break;
    }
}

// TODO(justforlxz): 错误信息应该存放在User类中, 切换用户后其他控件读取错误信息，而不是在这里分发。
void GreeterWorkek::message(QString text, QLightDM::Greeter::MessageType type)
{
    qDebug() << "pam message: " << text << type;

    if (text == "Verification timed out") {
        m_isThumbAuth = true;

        //V20版本新需求：若用户输入了密码，当指纹解锁超时，自动校验一次密码登录
        if (!m_password.isEmpty()) {
            QTimer::singleShot(300, this, [ = ] {
                m_greeter->respond(m_password);
            });
        }

        return;
    }

    switch (type) {
    case QLightDM::Greeter::MessageTypeInfo:
        if (m_isThumbAuth) break;
        emit m_model->authFaildMessage(QString(dgettext("fprintd", text.toUtf8())));
        FrameDataBind::Instance()->updateValue("deepinAuthMsg", text);
        break;

    case QLightDM::Greeter::MessageTypeError:
        emit m_model->authFaildTipsMessage(QString(dgettext("fprintd", text.toUtf8())));
        break;
    }
}

void GreeterWorkek::authenticationComplete()
{
    qDebug() << "authentication complete, authenticated " << m_greeter->isAuthenticated();
    if (!m_authSuccess) {
        m_authSuccess = m_greeter->isAuthenticated();
    }

    emit m_model->authFinished(m_greeter->isAuthenticated());

    if (m_greeter->isAuthenticated()) {
        if (m_showAuthResult) {
            // s5一键开机登陆时需要显示认证成功
            m_showAuthResult = false;
            QTimer::singleShot(1200, this, [ = ] {
                emit m_model->authFinished(true);
            });
        } else {
            emit m_model->authFinished(true);
        }
    } else {
        emit m_model->authFinished(false);
    }

    if (!m_greeter->isAuthenticated()) {
        m_authenticating = false;
        if (m_password.isEmpty()) {
            qDebug() << "Request GreeterWorkek::authenticationComplete -- fingerprint auth fail" << m_model->currentUser()->name();
            resetLightdmAuth(m_model->currentUser(), 100, false);
            return;
        }

        m_password.clear();

        if (m_model->currentUser()->type() == User::Native) {
            emit m_model->authFaildTipsMessage(tr("Wrong Password"));
        }

        if (m_model->currentUser()->type() == User::ADDomain) {
            emit m_model->authFaildTipsMessage(tr("The account or password is not correct. Please enter again."));
        }

        if (m_model->currentUser()->isLockForNum()) {
            m_model->currentUser()->startLock();
            return;
        }

        qDebug() << "Request GreeterWorkek::authenticationComplete -- password auth fail" << m_model->currentUser()->name();
        resetLightdmAuth(m_model->currentUser(), 100, false);

        return;
    }

    switch (m_model->powerAction()) {
    case SessionBaseModel::PowerAction::RequireRestart:
        m_login1Inter->Reboot(true);
        return;
    case SessionBaseModel::PowerAction::RequireShutdown:
        m_login1Inter->PowerOff(true);
        return;
    default: break;
    }

    qDebug() << "start session = " << m_model->sessionKey();

    auto startSessionSync = [ = ]() {
        QJsonObject json;
        json["Uid"]  = static_cast<int>(m_model->currentUser()->uid());
        json["Type"] = m_model->currentUser()->type();
        m_lockInter->SwitchToUser(QString(QJsonDocument(json).toJson(QJsonDocument::Compact))).waitForFinished();

        m_greeter->startSessionSync(m_model->sessionKey());
        m_authenticating = false;
    };

    // NOTE(kirigaya): It is not necessary to display the login animation.
    emit requestUpdateBackground(m_model->currentUser()->desktopBackgroundPath());

    if (m_firstTimeLoginForOneKey) {m_firstTimeLoginForOneKey = false;}

#ifndef DISABLE_LOGIN_ANI
    QTimer::singleShot(1200, this, startSessionSync);
#else
    startSessionSync();
#endif
}

void GreeterWorkek::saveNumlockStatus(std::shared_ptr<User> user, const bool &on)
{
    UserNumlockSettings(user->name()).set(on);
}

void GreeterWorkek::recoveryUserKBState(std::shared_ptr<User> user)
{
    //FIXME(lxz)
    //    PowerInter powerInter("com.deepin.system.Power", "/com/deepin/system/Power", QDBusConnection::systemBus(), this);
    //    const BatteryPresentInfo info = powerInter.batteryIsPresent();
    //    const bool defaultValue = !info.values().first();
    const bool enabled = UserNumlockSettings(user->name()).get(false);

    qDebug() << "restore numlock status to " << enabled;

    // Resync numlock light with numlock status
    bool cur_numlock = KeyboardMonitor::instance()->isNumlockOn();
    KeyboardMonitor::instance()->setNumlockStatus(!cur_numlock);
    KeyboardMonitor::instance()->setNumlockStatus(cur_numlock);

    KeyboardMonitor::instance()->setNumlockStatus(enabled);
}

void GreeterWorkek::resetLightdmAuth(std::shared_ptr<User> user,int delay_time , bool is_respond)
{
        qDebug() << "suo: " << __func__ << __LINE__;
    if (user->isLock()) {return;}

    QTimer::singleShot(delay_time, this, [ = ] {
        qDebug() << "suo: authen" << m_greeter->authenticationUser() <<  m_greeter->inAuthentication() << user->name();
        if(!m_greeter->inAuthentication() || m_greeter->authenticationUser() != user->name()) {
            qDebug() << "suo: authen";
            m_greeter->authenticate(user->name());
            if (is_respond) {
                m_greeter->respond(m_password);
            }
        }
    });
}

void GreeterWorkek::screenSwitchByWldpms(bool active)
{
    qDebug() << "GreeterWorkek::screenSwitchByWldpms:" << active;
    QStringList arguments;
    arguments << "-s";
    if (active) {
        arguments << "on";
    } else {
        arguments << "off";
    }
    QProcess::startDetached("dde_wldpms", arguments);
}

void GreeterWorkek::callAuthForLightdm(const QString &user)
{
    qDebug() << __func__ << __LINE__ << user;
    const QJsonObject obj = QJsonDocument::fromJson(user.toUtf8()).object();
    m_currentUserUid = static_cast<uint>(obj["Uid"].toInt());
    //一键登录
    if (m_firstTimeLoginForOneKey) {
        return;
    }

    for (std::shared_ptr<User> user_ptr : m_model->userList()) {
        if (!user_ptr->isLogin() && user_ptr->uid() == m_currentUserUid) {
            m_model->setCurrentUser(user_ptr);
            qDebug() << "suo: callAuth"<< __func__ << __LINE__ << user_ptr->name();
            userAuthForLightdm(user_ptr);
            break;
        }
    }
}
