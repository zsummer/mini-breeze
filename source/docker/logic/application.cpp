﻿#include "../config.h"
#include "application.h"
#include "dbService.h"
#include "userMgrService.h"
#include "userService.h"
#include "shellService.h"
#include <ProtoUser.h>
int g_closeState = 0;

Docker::Docker()
{

}

bool Docker::init(const std::string & config, DockerID idx)
{
    if (!ServerConfig::getRef().parse(config, idx))
    {
        LOGE("Docker::init error. parse config error. config path=" << config << ", docker ID = " << idx);
        return false;
    }
    if (idx == InvalidDockerID)
    {
        LOGE("Docker::init error. current docker id invalid. config path=" << config << ", docker ID = " << idx);
        return false;
    }
    const auto & dockers = ServerConfig::getRef().getDockerConfig();
    auto founder = std::find_if(dockers.begin(), dockers.end(), [](const DockerConfig& cc){return cc._docker == ServerConfig::getRef().getDockerID(); });
    if (founder == dockers.end())
    {
        LOGE("Docker::init error. current docker id not found in config file.. config path=" << config << ", docker ID = " << idx);
        return false;
    }
    LOGA("Docker::init  success. dockerID=" << idx);
    SessionManager::getRef().createTimer(1000, std::bind(&Docker::checkServiceState, Docker::getPtr()));
    return true;
}
void sigInt(int sig)
{
    if (g_closeState == 0)
    {
        g_closeState = 1;
        SessionManager::getRef().post(std::bind(&Docker::stop, Docker::getPtr()));
    }
}

void Docker::onCheckSafeExit()
{
    LOGA("Docker::onCheckSafeExit. checking.");
    if (_wlisten != InvalidAccepterID)
    {
        auto &options = SessionManager::getRef().getAccepterOptions(_wlisten);
        if (options._currentLinked != 0)
        {
            SessionManager::getRef().createTimer(1000, std::bind(&Docker::onCheckSafeExit, this));
            return;
        }
    }
    if (g_closeState == 1)
    {
        g_closeState = 2;
        for (auto &second : _services)
        {
            if (second.first == ServiceClient || second.first == ServiceInvalid)
            {
                continue;
            }
            for (auto & svc : second.second)
            {
                if (!svc.second->isShell())
                {
                    svc.second->onUninit(); //需要逐依赖关系uninit  
                }
            }
        }
    }

    bool safe =  true;
    for(auto &second : _services)
    {
        if (second.first == ServiceClient || second.first == ServiceUser || second.first == ServiceInvalid)
        {
            continue;
        }
        for (auto & svc : second.second)
        {
            if (!svc.second->isShell() && svc.second->getStatus() == SS_WORKING)   //状态判断需要改  
            {
                safe = false;
            }
        }
    }
    if(safe)
    {
        LOGA("Docker::onNetworkStoped. service all closed.");
        SessionManager::getRef().stopAccept();
        SessionManager::getRef().kickClientSession();
        SessionManager::getRef().kickConnect();
        SessionManager::getRef().stop();
    }
    else
    {
        SessionManager::getRef().createTimer(1000, std::bind(&Docker::onCheckSafeExit, this));
    }
}
bool Docker::run()
{
    SessionManager::getRef().run();
    LOGA("Docker::run exit!");
    return true;
}

bool Docker::isStoping()
{
    return g_closeState != 0;
}

void Docker::stop()
{
    SessionManager::getRef().stopAccept();
    if (_wlisten != InvalidAccepterID)
    {
        SessionManager::getRef().kickClientSession(_wlisten);
    }
    onCheckSafeExit();
    return ;
}

bool Docker::startDockerListen()
{
    const auto & dockers = ServerConfig::getRef().getDockerConfig();
    ServerConfig::getRef().getDockerID();
    auto founder = std::find_if(dockers.begin(), dockers.end(), [](const DockerConfig& cc){return cc._docker == ServerConfig::getRef().getDockerID(); });
    if (founder == dockers.end())
    {
        LOGE("Docker::startDockerListen error. current docker id not found in config file." );
        return false;
    }
    const DockerConfig & docker = *founder;
    if (docker._serviceBindIP.empty() || docker._servicePort == 0)
    {
        LOGE("Docker::startDockerListen check config error. bind ip=" << docker._serviceBindIP << ", bind port=" << docker._servicePort);
        return false;
    }
    AccepterID aID = SessionManager::getRef().addAccepter(docker._serviceBindIP, docker._servicePort);
    if (aID == InvalidAccepterID)
    {
        LOGE("Docker::startDockerListen addAccepter error. bind ip=" << docker._serviceBindIP << ", bind port=" << docker._servicePort);
        return false;
    }
    auto &options = SessionManager::getRef().getAccepterOptions(aID);
    options._whitelistIP = docker._whiteList;
    options._maxSessions = 1000;
    options._sessionOptions._sessionPulseInterval = 5000;
    options._sessionOptions._onSessionPulse = [](TcpSessionPtr session)
    {
        DockerPulse pulse;
        WriteStream ws(pulse.getProtoID());
        ws << pulse;
        session->send(ws.getStream(), ws.getStreamLen());
    };
    options._sessionOptions._onBlockDispatch = std::bind(&Docker::event_onServiceMessage, this, _1, _2, _3);
    if (!SessionManager::getRef().openAccepter(aID))
    {
        LOGE("Docker::startDockerListen openAccepter error. bind ip=" << docker._serviceBindIP << ", bind port=" << docker._servicePort);
        return false;
    }
    LOGA("Docker::startDockerListen openAccepter success. bind ip=" << docker._serviceBindIP << ", bind port=" << docker._servicePort <<", aID=" << aID);
    return true;
}
bool Docker::startDockerConnect()
{
    const auto & dockers = ServerConfig::getRef().getDockerConfig();
    for (const auto & docker : dockers)
    {
        SessionID cID = SessionManager::getRef().addConnecter(docker._serviceIP, docker._servicePort);
        if (cID == InvalidSessionID)
        {
            LOGE("Docker::startDockerConnect addConnecter error. remote ip=" << docker._serviceIP << ", remote port=" << docker._servicePort);
            return false;
        }
        auto session = SessionManager::getRef().getTcpSession(cID);
        if (!session)
        {
            LOGE("Docker::startDockerConnect addConnecter error.  not found connect session. remote ip=" << docker._serviceIP << ", remote port=" << docker._servicePort << ", cID=" << cID);
            return false;
        }
        auto &options = session->getOptions();
        options._onSessionLinked = std::bind(&Docker::event_onServiceLinked, this, _1);
        options._onSessionClosed = std::bind(&Docker::event_onServiceClosed, this, _1);
        options._onBlockDispatch = std::bind(&Docker::event_onServiceMessage, this, _1, _2, _3);
        options._reconnects = 50;
        options._connectPulseInterval = 5000;
        options._reconnectClean = false;
        options._onSessionPulse = [](TcpSessionPtr session)
        {
            auto last = session->getUserParamNumber(UPARAM_LAST_ACTIVE_TIME);
            if (last != 0 && getNowTime() - (time_t)last > 30000)
            {
                LOGE("options._onSessionPulse check timeout failed. diff time=" << getNowTime() - (time_t)last);
                session->close();
            }
        };

        if (!SessionManager::getRef().openConnecter(cID))
        {
            LOGE("Docker::startDockerConnect openConnecter error. remote ip=" << docker._serviceIP << ", remote port=" << docker._servicePort << ", cID=" << cID);
            return false;
        }
        LOGA("Docker::startDockerConnect success. remote ip=" << docker._serviceIP << ", remote port=" << docker._servicePort << ", cID=" << cID);
        session->setUserParam(UPARAM_SESSION_STATUS, SSTATUS_TRUST);
        session->setUserParam(UPARAM_REMOTE_CLUSTER, docker._docker);
        auto &ds = _dockerSession[docker._docker];
        ds.dokerID = docker._docker;
        ds.sessionID = cID;
        for (ui16 i = ServiceInvalid; i <= ServiceMax; i++)
        {
            _services.insert(std::make_pair(i, std::unordered_map<ServiceID, ServicePtr >()));
        }
        for (auto st : docker._services)
        {
            if (!createService(st, InvalidServiceID, docker._docker, docker._docker != ServerConfig::getRef().getDockerID(), InvalidSessionID,true))
            {
                return false;
            }
        }
    }
    return true;
}
bool Docker::startDockerWideListen()
{
    const auto & dockers = ServerConfig::getRef().getDockerConfig();
    ServerConfig::getRef().getDockerID();
    auto founder = std::find_if(dockers.begin(), dockers.end(), [](const DockerConfig& cc){return cc._docker == ServerConfig::getRef().getDockerID(); });
    if (founder == dockers.end())
    {
        LOGE("Docker::startDockerWideListen error. current docker id not found in config file.");
        return false;
    }
    const DockerConfig & docker = *founder;
    if (!docker._wideIP.empty() && docker._widePort != 0)
    {
        AccepterID aID = SessionManager::getRef().addAccepter("0.0.0.0", docker._widePort);
        if (aID == InvalidAccepterID)
        {
            LOGE("Docker::startDockerWideListen addAccepter error. bind ip=0.0.0.0, show wide ip=" << docker._wideIP << ", bind port=" << docker._widePort);
            return false;
        }
        auto &options = SessionManager::getRef().getAccepterOptions(aID);
        options._whitelistIP = docker._whiteList;
        options._maxSessions = 5000;
        options._sessionOptions._sessionPulseInterval = 10000;
        options._sessionOptions._onSessionPulse = [](TcpSessionPtr session)
        {
            auto last = session->getUserParamNumber(UPARAM_LAST_ACTIVE_TIME);
            if (getNowTime() - (time_t)last > 45000)
            {
                LOGE("options._onSessionPulse check timeout failed. diff time=" << getNowTime() - (time_t)last);
                session->close();
            }
        };
        options._sessionOptions._onSessionLinked = std::bind(&Docker::event_onClientLinked, this, _1);
        options._sessionOptions._onSessionClosed = std::bind(&Docker::event_onClientClosed, this, _1);
        options._sessionOptions._onBlockDispatch = std::bind(&Docker::event_onClientMessage, this, _1, _2, _3);
        if (!SessionManager::getRef().openAccepter(aID))
        {
            LOGE("Docker::startDockerWideListen openAccepter error. bind ip=0.0.0.0, show wide ip=" << docker._wideIP << ", bind port=" << docker._widePort);
            return false;
        }
        LOGA("Docker::startDockerWideListen openAccepter success. bind ip=0.0.0.0, show wide ip=" << docker._wideIP << ", bind port=" << docker._widePort << ", listen aID=" << aID);
        _wlisten = aID;
    }
    return true;
}

bool Docker::start()
{
    return startDockerListen() && startDockerConnect() && startDockerWideListen();
}






void Docker::event_onServiceLinked(TcpSessionPtr session)
{
    DockerID ci = (DockerID)session->getUserParamNumber(UPARAM_REMOTE_CLUSTER);
    auto founder = _dockerSession.find(ci);
    if (founder == _dockerSession.end())
    {
        LOGE("event_onServiceLinked error cID=" << session->getSessionID() << ", dockerID=" << ci);
        return;
    }
    LOGI("event_onServiceLinked cID=" << session->getSessionID() << ", dockerID=" << ci);
    founder->second.status = 1;
    for (auto & second : _services)
    {
        if (second.first == ServiceClient || second.first == ServiceUser || second.first == ServiceInvalid)
        {
            continue;
        }
        for (auto & svc : second.second )
        {
            if (!svc.second->isShell() && svc.second->getStatus() == SS_WORKING)
            {
                CreateOrRefreshServiceNotice notice(svc.second->getServiceType(), svc.second->getServiceID(), svc.second->getDockerID(), svc.second->getClientID());
                Docker::getRef().sendToSession(session->getSessionID(), notice);
            }
        }
    }
    checkServiceState();
}

void Docker::event_onServiceClosed(TcpSessionPtr session)
{
    DockerID ci = (DockerID)session->getUserParamNumber(UPARAM_REMOTE_CLUSTER);
    auto founder = _dockerSession.find(ci);
    if (founder == _dockerSession.end())
    {
        LOGE("event_onServiceClosed error cID=" << session->getSessionID() << ", dockerID=" << ci);
        return;
    }
    LOGW("event_onServiceClosed cID=" << session->getSessionID() << ", dockerID=" << ci);
    founder->second.status = 0;
}

void Docker::destroyService(ui16 serviceType, ServiceID serviceID)
{
    auto founder = _services.find(serviceType);
    if (founder == _services.end())
    {
        return;
    }
    auto fder = founder->second.find(serviceID);
    if (fder == founder->second.end())
    {
        return;
    }
    founder->second.erase(fder);
    return;
}

ServicePtr Docker::createService(ui16 serviceType, ServiceID serviceID, DockerID dockerID, SessionID clientID, bool isShell, bool failExit)
{
    ServicePtr & service = _services[serviceType][serviceID];
    if (service && !service->isShell())
    {
        LOGE("Docker::createService error. service alread exist. serviceType=" << serviceType << ", serviceID="
            << serviceID << ", dockerID=" << dockerID << ", isShell=" << isShell << ", failExit=" << failExit);
        if (failExit)
        {
            goto goExit;
        }
        return nullptr;
    }
    if (isShell)
    {
        service = std::make_shared<ShellService>();
        service->setShell(isShell);
    }
    else if (serviceType == ServiceDictDBMgr)
    {
        service = std::make_shared<DBService>();
    }
    else if (serviceType == ServiceInfoDBMgr)
    {
        service = std::make_shared<DBService>();
    }
    else if (serviceType == ServiceLogDBMgr)
    {
        service = std::make_shared<DBService>();
    }
    else if (serviceType == ServiceUserMgr)
    {
        service = std::make_shared<UserMgrService>();
    }
    else if (serviceType == ServiceUser)
    {
        service = std::make_shared<UserService>();
    }
    else
    {
        LOGE("createService invalid service type " << serviceType);
        if (failExit)
        {
            goto goExit;
        }
        return nullptr;
    }
    service->setServiceType(serviceType);
    service->setServiceID(serviceID);
    service->setDockerID(dockerID);
    service->setClientID(clientID);
    if (isShell)
    {
        service->setStatus(SS_WORKING);
    }
    else
    {
        service->setStatus(SS_CREATED);
    }
    
    return service;
goExit:
    Docker::getRef().stop();
    return nullptr;
}

void Docker::checkServiceState()
{
    SessionManager::getRef().createTimer(1000, std::bind(&Docker::checkServiceState, Docker::getPtr()));
    if (!_dockerNetWorking)
    {
        for (auto & c : _dockerSession)
        {
            if (c.second.status == 0)
            {
                return;
            }
        }
        _dockerNetWorking = true;
        LOGA("docker net worked");
    }

    for (auto & second : _services)
    {
        for (auto service : second.second)
        {
            if (service.second && !service.second->isShell() && service.second->getServiceType() != ServiceUser 
                && (service.second->getStatus() == SS_INITING || service.second->getStatus() == SS_WORKING || service.second->getStatus() == SS_UNINITING) )
            {
                service.second->onTick();
            }
        }
    }
    if (!_dockerServiceWorking)
    {
        for (ui16 i = ServiceInvalid+1; i != ServiceMulti; i++)
        {
            auto founder = _services.find(i);
            if (founder == _services.end())
            {
                LOGE("not found service id=" << i);
                continue;
            }
            for (auto service : founder->second)
            {
                if (!service.second)
                {
                    LOGE("local service nulptr ...");
                    Docker::getRef().stop();
                    return;
                }
                if (service.second->isShell() && service.second->getStatus() != SS_WORKING)
                {
                    LOGD("waiting shell service[" << service.second->getServiceType() << "] working.. ");
                    return;
                }
                if (!service.second->isShell() && service.second->getStatus() == SS_INITING)
                {
                    LOGD("waiting local service[" << service.second->getServiceType() << "] initing.. ");
                    return;
                }
                if (!service.second->isShell() && service.second->getStatus() == SS_CREATED)
                {
                    LOGI("local service [" << ServiceNames.at(service.second->getServiceType()) << "] begin init. [" << service.second->getServiceID() << "] ...");
                    service.second->setStatus(SS_INITING);
                    bool ret = service.second->onInit();
                    if (ret)
                    {
                        LOGI("local service [" << ServiceNames.at(service.second->getServiceType()) << "] inited. [" << service.second->getServiceID() << "] ...");
                    }
                    else
                    {
                        LOGE("local service [" << ServiceNames.at(service.second->getServiceType()) << "]  init error.[" << service.second->getServiceID() << "] ...");
                        Docker::getRef().stop();
                        return;
                    }
                    return;
                }
                if (service.second->getStatus() == SS_DESTROY || service.second->getStatus() == SS_UNINITING)
                {
                    LOGE("local service [" << ServiceNames.at(service.second->getServiceType()) << "]  init error.[" << service.second->getServiceID() << "] ...");
                    Docker::getRef().stop();
                    return;
                }
            }
        }
        LOGA("all service worked.");
        _dockerServiceWorking = true;

    }
   
}
void Docker::event_onForwardToDocker(TcpSessionPtr session, ReadStream & rs)
{
    Tracing trace;
    rs >> trace;
    auto founder = _services.find(trace._toService);
    if (founder == _services.end())
    {
        LOGE("Docker::event_onRemoteShellForward error. unknown service. trace=" << trace);
        return;
    }
    auto fder = founder->second.find(trace._toServiceID);
    if (fder == founder->second.end())
    {
        LOGE("Docker::event_onRemoteShellForward error. not found service id. trace=" << trace);
        return;
    }
    if (fder->second->isShell())
    {
        LOGE("Docker::event_onRemoteShellForward error. service not local. trace=" << trace);
        return;
    }
    fder->second->process(trace, rs.getStreamUnread(), rs.getStreamUnreadLen());
}

void Docker::event_onCreateServiceInDocker(TcpSessionPtr session, ReadStream & rs)
{
    CreateServiceInDocker service;
    rs >> service;
    auto founder = _services.find(service.serviceType);
    if (founder == _services.end())
    {
        LOGE("CreateServiceInDocker can't founder service type. service=" << ServiceNames.at(service.serviceType));
        return;
    }
    auto ret = createService(service.serviceType, service.serviceID, ServerConfig::getRef().getDockerID(), service.clientID, false, false);
    if (ret)
    {
        ret->onInit();
    }
}

void Docker::event_onChangeServiceClientID(TcpSessionPtr session, ReadStream & rs)
{
    CreateServiceInDocker service;
    rs >> service;
    auto founder = _services.find(service.serviceType);
    if (founder == _services.end())
    {
        LOGE("event_onChangeServiceClientID can't founder service type. service=" << ServiceNames.at(service.serviceType));
        return;
    }
    auto fder = founder->second.find(service.serviceID);
    if (fder == founder->second.end() || fder->second->isShell())
    {
        LOGE("event_onChangeServiceClientID can't founder service id. service=" << ServiceNames.at(service.serviceType) << ", id=" << service.serviceID);
        return;
    }
    fder->second->setClientID(service.clientID);
    CreateOrRefreshServiceNotice notice(service.serviceType, service.serviceID, ServerConfig::getRef().getDockerID(), service.clientID);
    broadcast(notice, false);
}

void Docker::event_onCreateOrRefreshServiceNotice(TcpSessionPtr session, ReadStream & rs)
{
    CreateOrRefreshServiceNotice service;
    rs >> service;

    auto founder = _services.find(service.serviceType);
    if (founder == _services.end())
    {
        LOGE("event_onCreateOrRefreshServiceNotice can't founder remote service. service=" << ServiceNames.at(service.serviceType));
        return;
    }
    createService(service.serviceType, service.serviceID, service.dockerID, service.clientID, true, false);
}

void Docker::event_onDestroyServiceInDocker(TcpSessionPtr session, ReadStream & rs)
{
    DestroyServiceInDocker service;
    rs >> service;

    auto founder = _services.find(service.serviceType);
    if (founder == _services.end())
    {
        LOGE("event_onDestroyServiceInDocker can't founder remote service. service=" << ServiceNames.at(service.serviceType));
        return;
    }
    auto fder = founder->second.find(service.serviceID);
    if (fder != founder->second.end() && fder->second && fder->second->getStatus() == SS_WORKING && !fder->second->isShell())
    {
        fder->second->setStatus(SS_UNINITING);
        fder->second->onUninit();
    }
}

void Docker::event_onDestroyServiceNotice(TcpSessionPtr session, ReadStream & rs)
{
    DestroyServiceNotice service;
    rs >> service;

    auto founder = _services.find(service.serviceType);
    if (founder == _services.end())
    {
        LOGE("event_onDestroyServiceNotice can't founder remote service. service=" << ServiceNames.at(service.serviceType));
        return;
    }
    destroyService(service.serviceType, service.serviceID);
}


void Docker::event_onServiceMessage(TcpSessionPtr   session, const char * begin, unsigned int len)
{
    ReadStream rsShell(begin, len);

    if (rsShell.getProtoID() == DockerPulse::getProtoID())
    {
        session->setUserParam(UPARAM_LAST_ACTIVE_TIME, getNowTime());
        return;
    }
    else if (rsShell.getProtoID() == CreateServiceInDocker::getProtoID())
    {
        event_onCreateServiceInDocker(session, rsShell);
        return;
    }
    else if (rsShell.getProtoID() == ChangeServiceClientID::getProtoID())
    {
        event_onChangeServiceClientID(session, rsShell);
        return;
    }
    else if (rsShell.getProtoID() == CreateOrRefreshServiceNotice::getProtoID())
    {
        event_onCreateOrRefreshServiceNotice(session, rsShell);
        return;
    }
    else if (rsShell.getProtoID() == DestroyServiceInDocker::getProtoID())
    {
        event_onDestroyServiceInDocker(session, rsShell);
        return;
    }
    else if (rsShell.getProtoID() == DestroyServiceNotice::getProtoID())
    {
        event_onDestroyServiceNotice(session, rsShell);
        return;
    }
    else if (rsShell.getProtoID() == ForwardToDocker::getProtoID())
    {
        event_onForwardToDocker(session, rsShell);
        return;
    }
    
}




void Docker::event_onClientPulse(TcpSessionPtr session)
{
    if (isSessionID(session->getSessionID()))
    {
        if (time(NULL) - session->getUserParamNumber(UPARAM_LAST_ACTIVE_TIME) > session->getOptions()._sessionPulseInterval / 1000 * 2)
        {
            session->close();
            return;
        }
    }
}






void Docker::event_onClientLinked(TcpSessionPtr session)
{
    session->setUserParam(UPARAM_SESSION_STATUS, SSTATUS_UNKNOW);
    LOGD("Docker::event_onClientLinked. SessionID=" << session->getSessionID() 
        << ", remoteIP=" << session->getRemoteIP() << ", remotePort=" << session->getRemotePort());
}

void Docker::event_onClientClosed(TcpSessionPtr session)
{
    LOGD("Docker::event_onClientClosed. SessionID=" << session->getSessionID() 
        << ", remoteIP=" << session->getRemoteIP() << ", remotePort=" << session->getRemotePort());
    if (isConnectID(session->getSessionID()))
    {
    }
    else
    {
        if (session->getUserParamNumber(UPARAM_SESSION_STATUS) == SSTATUS_LOGINED)
        {

        }

    }
}



void Docker::event_onClientMessage(TcpSessionPtr session, const char * begin, unsigned int len)
{
    ReadStream rs(begin, len);
    SessionStatus ss = (SessionStatus) session->getUserParamNumber(UPARAM_SESSION_STATUS);
    if (rs.getProtoID() == ClientAuthReq::getProtoID())
    {
        LOGD("ClientAuthReq sID=" << session->getSessionID() << ", block len=" << len);
        if (ss != SSTATUS_UNKNOW)
        {
            LOGE("duplicate ClientAuthReq. sID=" << session->getSessionID());
            return;
        }
        ClientAuthReq careq;
        rs >> careq;
        Tracing trace;
        trace._fromService = ServiceClient;
        trace._fromServiceID = session->getSessionID();
        trace._toService = ServiceUserMgr;
        trace._toServiceID = InvalidServiceID;
        WriteStream ws(UserAuthReq::getProtoID());
        UserAuthReq req;
        req.account = careq.account;
        req.token = careq.token;
        req.clientSessionID = session->getSessionID();
        req.clientDockerID = ServerConfig::getRef().getDockerID();
        ws << req;
        toService(trace, ws.getStream(), ws.getStreamLen(), true);
        session->setUserParam(UPARAM_SESSION_STATUS, SSTATUS_AUTHING);
        return;
    }

}

void Docker::sendToDocker(DockerID dockerID, const char * block, unsigned int len)
{
    auto founder = _dockerSession.find(dockerID);
    if (founder == _dockerSession.end())
    {
        LOGF("Docker::sendToDocker fatal error. dockerID not found. dockerID=" << dockerID);
        return;
    }
    if (founder->second.sessionID == InvalidSessionID)
    {
        LOGF("Docker::sendToDocker fatal error. dockerID not have session. dockerID=" << dockerID);
        return;
    }
    if (founder->second.status == 0)
    {
        LOGW("Docker::sendToDocker warning error. session try connecting. dockerID=" << dockerID << ", client session ID=" << founder->second.sessionID);
    }
    sendToSession(founder->second.sessionID, block, len);
}

void Docker::toService(Tracing trace, const char * block, unsigned int len, bool isFromeOtherDocker)
{
    if (trace._fromService >= ServiceMax || trace._toService >= ServiceMax || trace._toService == ServiceInvalid)
    {
        LOGF("Docker::sendToService Illegality trace. trace=" << trace << ", block len=" << len);
        return;
    }

    ui16 toService = trace._toService;
    if (trace._toService == ServiceClient)
    {
        toService = ServiceUser;
    }

    auto founder = _services.find(toService);
    if (founder == _services.end())
    {
        LOGF("Docker::toService can not found _toService type  trace =" << trace << ", block len=" << len);
        return;
    }
    auto fder = founder->second.find(trace._toServiceID);
    if (fder == founder->second.end())
    {
        LOGD("Docker::toService can not found _toService ID. trace =" << trace << ", block len=" << len);
        return;
    }
    auto & service = *fder->second;
    if (service.isShell() && isFromeOtherDocker)
    {
        LOGE("local service is shell but the call from other docker.");
        return;
    }
    if (service.isShell()) //forward 
    {
        DockerID dockerID = service.getDockerID();
        forwardToDocker(dockerID, trace, block, len);
    }
    else //direct process
    {
        if (trace._toService == ServiceClient)
        {
            if (service.getClientID() != InvalidSessionID)
            {
                sendToSession(service.getClientID(), block, len);
            }
        }
        else if (isFromeOtherDocker)
        {
            fder->second->process(trace, block, len);
        }
        else
        {
            std::string bk;
            bk.assign(block, len);
            SessionManager::getRef().post(std::bind(&Service::process4bind, fder->second, trace, std::move(bk)));
        }
    }

}


void Docker::forwardToDocker(DockerID dockerID, const Tracing & trace, const char * block, unsigned int len)
{
    auto founder = _dockerSession.find(dockerID);
    if (founder != _dockerSession.end() && founder->second.sessionID != InvalidSessionID && founder->second.status != 0)
    {
        forwardToSession(founder->second.sessionID, trace, block, len);
    }
    else
    {
        LOGE("Docker::forwardToDocker not found docker. dockerID=" << dockerID);
    }
}



void Docker::sendToSession(SessionID sessionID, const char * block, unsigned int len)
{
    SessionManager::getRef().sendSessionData(sessionID, block, len);
}


void Docker::forwardToSession(SessionID sessionID, const Tracing & trace, const char * block, unsigned int len)
{
    try
    {
        WriteStream ws(ForwardToDocker::getProtoID());
        ws << trace;
        ws.appendOriginalData(block, len);
        sendToSession(sessionID, ws.getStream(), ws.getStreamLen());
    }
    catch (const std::exception & e)
    {
        LOGE("Docker::forwardToSession catch except error. e=" << e.what());
    }
}







