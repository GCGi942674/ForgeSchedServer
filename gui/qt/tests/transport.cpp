#include "RpcClient.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QEventLoop>
#include <QTimer>
#include <QtEndian>
#include <cstdio>

bool scenario(bool badId,bool badLength,bool timeout) {
    QTcpServer server;if(!server.listen(QHostAddress::LocalHost,0))return false;
    QObject context;QEventLoop loop;bool finished=false,success=false;
    QObject::connect(&server,&QTcpServer::newConnection,&context,[&]{
        auto socket=server.nextPendingConnection();auto buffer=std::make_shared<QByteArray>();
        QObject::connect(socket,&QTcpSocket::readyRead,&context,[&,socket,buffer]{
            buffer->append(socket->readAll());if(buffer->size()<4)return;
            auto size=qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(buffer->constData()));if(buffer->size()<int(size)+4)return;
            auto request=Json::parse(buffer->constData()+4,buffer->constData()+4+size);
            if(timeout)return;
            auto id=request.at("request_id").get<uint64_t>();
            auto body=Json{{"version",1},{"type","response"},{"request_id",badId?id+1:id},
                          {"data",{{"code",0},{"result",{{"task_id",uint64_t(9007199254740993ULL)}}}}}}.dump();
            QByteArray frame(4,'\0');qToBigEndian<quint32>(badLength?1048577:quint32(body.size()),reinterpret_cast<uchar*>(frame.data()));frame.append(body.data(),int(body.size()));
            socket->write(frame.left(2));QTimer::singleShot(5,socket,[socket,frame]{socket->write(frame.mid(2));});
        });
    });
    RpcClient client;client.configure("127.0.0.1",server.serverPort(),100);
    client.request("query_status",Json::object(),[&](RpcReply r){finished=true;success=(badId||badLength||timeout)?!r.ok:r.ok&&r.result.at("task_id")==uint64_t(9007199254740993ULL);loop.quit();});
    QTimer::singleShot(2000,&loop,&QEventLoop::quit);loop.exec();return finished&&success;
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);bool ok=scenario(false,false,false)&&scenario(true,false,false)&&scenario(false,true,false)&&scenario(false,false,true);std::fprintf(stderr,"QT_TRANSPORT_TEST %s\n",ok?"PASS":"FAIL");return ok?0:1;}
