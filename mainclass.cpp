#include "mainclass.h"

#include <functional>

QList<_QSCD_FOR_MULTIMAP> prexPgaList;
int prexDataEpochTime;

QMultiMap<int, _QSCD_FOR_MULTIMAP> QSCD_DATA_HOUSE;
QList<_STATION> staList;

MainClass::MainClass(QString configFileName, QObject *parent) : QObject(parent)
{
    activemq::library::ActiveMQCPP::initializeLibrary();

    cfg = readCFG(configFileName);
    qRegisterMetaType< QMultiMap<int, _QSCD_FOR_MULTIMAP> >("QMultiMap<int,_QSCD_FOR_MULTIMAP>");

    initProj();

    writeLog(cfg.logDir, cfg.processName, "======================================================");

    qscdDB = QSqlDatabase::addDatabase("QMYSQL");
    qscdDB.setHostName(cfg.db_ip);
    qscdDB.setDatabaseName(cfg.db_name);
    qscdDB.setUserName(cfg.db_user);
    qscdDB.setPassword(cfg.db_passwd);

    networkModel = new QSqlQueryModel();
    affiliationModel = new QSqlQueryModel();
    siteModel = new QSqlQueryModel();

    readStationListfromDB();

    QSCD_DATA_HOUSE.clear();

    QTimer *systemTimer = new QTimer;
    connect(systemTimer, SIGNAL(timeout()), this, SLOT(doRepeatWork()));
    systemTimer->start(1000);

    // consumer
    if(cfg.qscd20_kiss_amq_topic != "")
    {
        QString failover = "failover:(tcp://" + cfg.qscd20_kiss_amq_ip + ":" + cfg.qscd20_kiss_amq_port + ")";

        rvKISS_QSCD20_Thread = new RecvQSCD20Message;
        if(!rvKISS_QSCD20_Thread->isRunning())
        {
            rvKISS_QSCD20_Thread->setup(failover, cfg.qscd20_kiss_amq_user, cfg.qscd20_kiss_amq_passwd,
                                         cfg.qscd20_kiss_amq_topic, true, false, "KISS", cfg.staType);
            connect(rvKISS_QSCD20_Thread, SIGNAL(sendQSCDtoMain(QMultiMap<int, _QSCD_FOR_MULTIMAP>)),
                    this, SLOT(extractQSCD(QMultiMap<int, _QSCD_FOR_MULTIMAP>)));
            rvKISS_QSCD20_Thread->start();
        }
    }

    if(cfg.qscd20_mpss_amq_topic != "")
    {
        QString failover = "failover:(tcp://" + cfg.qscd20_mpss_amq_ip + ":" + cfg.qscd20_mpss_amq_port + ")";

        rvMPSS_QSCD20_Thread = new RecvQSCD20Message;
        if(!rvMPSS_QSCD20_Thread->isRunning())
        {
            rvMPSS_QSCD20_Thread->setup(failover, cfg.qscd20_mpss_amq_user, cfg.qscd20_mpss_amq_passwd,
                                         cfg.qscd20_mpss_amq_topic, true, false, "MPSS", cfg.staType);
            connect(rvMPSS_QSCD20_Thread, SIGNAL(sendQSCDtoMain(QMultiMap<int, _QSCD_FOR_MULTIMAP>)),
                    this, SLOT(extractQSCD(QMultiMap<int, _QSCD_FOR_MULTIMAP>)));
            rvMPSS_QSCD20_Thread->start();
        }
    }

    writeLog(cfg.logDir, cfg.processName, "QSCD202WSOCK Started : " + cfg.processName + ", Station Type : " + cfg.staType);


    m_pWebSocketServer = new QWebSocketServer(QStringLiteral("QSCD202WSOCK"),
                                              QWebSocketServer::NonSecureMode,  this);

    if(m_pWebSocketServer->listen(QHostAddress::Any, cfg.websocketPort))
    {
        writeLog(cfg.logDir, cfg.processName, "Listening on port : " + QString::number(cfg.websocketPort));

        connect(m_pWebSocketServer, &QWebSocketServer::newConnection,
                this, &MainClass::onNewConnection);
        connect(m_pWebSocketServer, &QWebSocketServer::closed,
                this, &QCoreApplication::quit);
    }
}

void MainClass::onNewConnection()
{
    QWebSocket *pSocket = m_pWebSocketServer->nextPendingConnection();
    connect(pSocket, &QWebSocket::disconnected, this, &MainClass::socketDisconnected);
    m_clients << pSocket;

    ProcessQSCDThread *prThread = new ProcessQSCDThread(pSocket);
    if(!prThread->isRunning())
    {
        prThread->start();
        connect(pSocket, &QWebSocket::disconnected, prThread, &ProcessQSCDThread::quit);
        connect(pSocket, &QWebSocket::textMessageReceived, prThread, &ProcessQSCDThread::recvTextMessage);
        connect(prThread, &ProcessQSCDThread::finished, prThread, &ProcessQSCDThread::deleteLater);
    }
}

void MainClass::socketDisconnected()
{
    QWebSocket *pClient = qobject_cast<QWebSocket *>(sender());

    if(pClient){
        m_clients.removeAll(pClient);
        pClient->deleteLater();
    }
}

void MainClass::extractQSCD(QMultiMap<int, _QSCD_FOR_MULTIMAP> mmFromAMQ)
{
    QMapIterator<int, _QSCD_FOR_MULTIMAP> i(mmFromAMQ);

    mutex.lock();
    while(i.hasNext())
    {
        i.next();
        QSCD_DATA_HOUSE.insert(i.key(), i.value());
    }
    mutex.unlock();
}

void MainClass::doRepeatWork()
{
    QDateTime systemTimeUTC = QDateTime::currentDateTimeUtc();
    QDateTime dataTimeUTC = systemTimeUTC.addSecs(- SECNODS_FOR_ALIGN_QSCD); // GMT

    mutex.lock();

    if(!QSCD_DATA_HOUSE.isEmpty())
    {
        QMultiMap<int, _QSCD_FOR_MULTIMAP>::iterator iter;
        QMultiMap<int, _QSCD_FOR_MULTIMAP>::iterator untilIter;
        untilIter = QSCD_DATA_HOUSE.lowerBound(dataTimeUTC.toTime_t() - KEEP_LARGE_DATA_DURATION);

        for(iter = QSCD_DATA_HOUSE.begin() ; untilIter != iter;)
        {
            QMultiMap<int, _QSCD_FOR_MULTIMAP>::iterator thisIter;
            thisIter = iter;
            iter++;
            QSCD_DATA_HOUSE.erase(thisIter);
        }
    }
    mutex.unlock();
}

void MainClass::openDB()
{
    if(!qscdDB.isOpen())
    {
        if(!qscdDB.open())
        {
            writeLog(cfg.logDir, cfg.processName, "Error connecting to DB: " + qscdDB.lastError().text());
        }
    }
}

void MainClass::readStationListfromDB()
{
    staList.clear();

    QString query;
    query = "SELECT * FROM NETWORK";
    openDB();
    networkModel->setQuery(query);

    if(networkModel->rowCount() > 0)
    {
        for(int i=0;i<networkModel->rowCount();i++)
        {
            QString network = networkModel->record(i).value("net").toString();
            query = "SELECT * FROM AFFILIATION where net='" + network + "'";
            affiliationModel->setQuery(query);

            for(int j=0;j<affiliationModel->rowCount();j++)
            {
                QString affiliation = affiliationModel->record(j).value("aff").toString();
                QString affiliationName = affiliationModel->record(j).value("affname").toString();
                float lat = affiliationModel->record(j).value("lat").toDouble();
                float lon = affiliationModel->record(j).value("lon").toDouble();
                query = "SELECT * FROM SITE where aff='" + affiliation + "'";
                siteModel->setQuery(query);

                for(int k=0;k<siteModel->rowCount();k++)
                {
                    if(!siteModel->record(k).value("statype").toString().startsWith(cfg.staType.left(1)))
                        continue;

                    _STATION sta;
                    QString staS = siteModel->record(k).value("sta").toString();    
                    strcpy(sta.netSta, staS.toLatin1().constData());
                    //sprintf(sta.netSta, "%s", (const char *)((QByteArray)(staS.toStdString().data())));
                    sta.lat = lat;
                    sta.lon = lon;
                    ll2xy4Small(pj_longlat, pj_eqc, sta.lon, sta.lat, &sta.smapX, &sta.smapY);
                    ll2xy4Large(pj_longlat, pj_eqc, sta.lon, sta.lat, &sta.lmapX, &sta.lmapY);
                    sta.inUse = siteModel->record(k).value("inuse").toInt();
                    if(sta.inUse != 1)
                        continue;
                    sta.pga[0] = 0;sta.pga[1] = 0;sta.pga[2] = 0;sta.pga[3] = 0;sta.pga[4] = 0;
                    sta.pgaTime = 0;
                    staList.append(sta);
                }
            }
        }
    }

    writeLog(cfg.logDir, cfg.processName, "Succedd Reading Station List from Database");
    writeLog(cfg.logDir, cfg.processName, "The number of the Stations : " + QString::number(staList.size()));
}

void MainClass::initProj()
{
    if (!(pj_longlat = pj_init_plus("+proj=longlat +ellps=WGS84")) )
    {
        qDebug() << "Can't initialize projection.";
        exit(1);
    }

    if (!(pj_eqc = pj_init_plus("+proj=eqc +ellps=WGS84")) )
    {
        qDebug() << "Can't initialize projection.";
        exit(1);
    }
}




ProcessQSCDThread::ProcessQSCDThread(QWebSocket *socket, QWidget *parent)
{
    pSocket = socket;
}

ProcessQSCDThread::~ProcessQSCDThread()
{
}

void ProcessQSCDThread::recvTextMessage(QString message)
{
    if(message.startsWith("Hello"))
        return;

    _BINARY_PGA_PACKET mypacket = generateData(message.toInt());
    sendBinaryMessage(mypacket);
}

_BINARY_PGA_PACKET ProcessQSCDThread::generateData(int dt)
{
    QList<_QSCD_FOR_MULTIMAP> pgaList = QSCD_DATA_HOUSE.values(dt);

    fillPGAintoStaList_Parallel(pgaList, dt);

    _BINARY_PGA_PACKET mypacket;

    mypacket.dataTime = dt;

    int realQSCDsize = 0;
    for(int i=0;i<staList.size();i++)
    {
        _STATION station = staList.at(i);
        if(station.pgaTime == dt)
        {
            mypacket.staList[realQSCDsize] = station;
            realQSCDsize++;
        }
    }

    mypacket.numStation = realQSCDsize;

    return mypacket;
}

void ProcessQSCDThread::fillPGAintoStaList_Parallel(QList<_QSCD_FOR_MULTIMAP> pgaList, int dataEpochTime)
{
    prexPgaList = pgaList;
    prexDataEpochTime = dataEpochTime;

    std::function<void(_STATION&)> fillPGAintoStaList = [](_STATION &station)
    {
        for(int i=0;i<prexPgaList.size();i++)
        {
            _QSCD_FOR_MULTIMAP qfmm = prexPgaList.at(i);

            if(QString(station.netSta).startsWith(qfmm.netSta))
            {
                for(int j=0;j<5;j++)
                    station.pga[j] = qfmm.pga[j];
                station.pgaTime = prexDataEpochTime;
                break;
            }
        }
    };

    QFuture<void> future = QtConcurrent::map(staList, fillPGAintoStaList);
    future.waitForFinished();
}

void ProcessQSCDThread::sendBinaryMessage(_BINARY_PGA_PACKET mypacket)
{
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.writeRawData((char*)&mypacket, sizeof(_BINARY_PGA_PACKET));

    pSocket->sendBinaryMessage(data);
}
