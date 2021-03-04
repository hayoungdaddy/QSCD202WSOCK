#ifndef MAINCLASS_H
#define MAINCLASS_H

#include <QObject>
#include <QTimer>
#include <QtConcurrent>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlQueryModel>
#include <QSqlRecord>
#include <QSqlError>

#include <QtWebSockets/QtWebSockets>
#include <QMutex>
#include <QDataStream>

#include "KGEEWLIBS_global.h"
#include "kgeewlibs.h"

#define QSCD202WSOCK_VERSION 0.1

class MainClass : public QObject
{
    Q_OBJECT
public:
    explicit MainClass(QString conFile = nullptr, QObject *parent = nullptr);

private:
    _CONFIGURE cfg;
    RecvQSCD20Message *rvKISS_QSCD20_Thread;
    RecvQSCD20Message *rvMPSS_QSCD20_Thread;

    QMutex mutex;

    void openDB();
    void readStationListfromDB();

    projPJ pj_eqc;
    projPJ pj_longlat;
    void initProj();

    // About Database & table
    QSqlDatabase qscdDB;
    QSqlQueryModel *networkModel;
    QSqlQueryModel *affiliationModel;
    QSqlQueryModel *siteModel;

    QWebSocketServer *m_pWebSocketServer;
    QList<QWebSocket *> m_clients;

private slots:
    void doRepeatWork();
    void extractQSCD(QMultiMap<int, _QSCD_FOR_MULTIMAP>);
    void onNewConnection();
    void socketDisconnected();
};



class ProcessQSCDThread : public QThread
{
    Q_OBJECT
public:
    ProcessQSCDThread(QWebSocket *websocket = nullptr, QWidget *parent = nullptr);
    ~ProcessQSCDThread();

public slots:
    void recvTextMessage(QString);

private:
    QWebSocket *pSocket;
    void fillPGAintoStaList_Parallel(QList<_QSCD_FOR_MULTIMAP>, int);
    _BINARY_PGA_PACKET generateData(int);

    void sendBinaryMessage(_BINARY_PGA_PACKET);
};

#endif // MAINCLASS_H
