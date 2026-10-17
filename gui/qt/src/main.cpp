#include "MainWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QHostAddress>
#include <cstdio>

namespace {
GuiSettings settings(const QCommandLineParser& parser) {
    if(!parser.positionalArguments().isEmpty())throw std::runtime_error("unexpected argument; use --ip ADDRESS --port PORT");
    if(!parser.isSet("ip")||!parser.isSet("port"))throw std::runtime_error("required: --ip ADDRESS --port PORT");
    GuiSettings result;result.host=parser.value("ip");QHostAddress address;
    if(!address.setAddress(result.host)||address.isNull()||address==QHostAddress(QHostAddress::AnyIPv4)||address==QHostAddress(QHostAddress::AnyIPv6))throw std::runtime_error("--ip must be a valid non-wildcard IPv4 or IPv6 address");
    const auto portText=parser.value("port");bool ok=false;const auto port=portText.toUInt(&ok);
    for(const auto c:portText)if(c<'0'||c>'9')ok=false;
    if(!ok||port<1||port>65535)throw std::runtime_error("--port must be an integer between 1 and 65535");
    result.port=quint16(port);return result;
}
}
int main(int argc,char** argv) {
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc,argv);app.setApplicationName("GCRPGui");app.setApplicationVersion("0.1");
    QCommandLineParser parser;parser.setApplicationDescription(QStringLiteral("GCRP C++ / Qt5 调度控制台"));
    parser.addHelpOption();parser.addVersionOption();
    parser.addOption({"demo",QStringLiteral("离线演示，不连接 Server")});
    parser.addOption({"self-test",QStringLiteral("自动检查窗口与目录树（需 --demo）")});
    parser.addOption({"ip",QStringLiteral("Server IP 地址（必填，不读取配置文件）"),"address"});
    parser.addOption({"port",QStringLiteral("Server TCP 端口（必填：1–65535）"),"port"});
    parser.addOption({"probe",QStringLiteral("只读检查 Server 能力、工作机和任务接口")});
    parser.addOption({"catalog-probe",QStringLiteral("只读验证 Server 目录 API 与实际 GUI 控件加载")});
    parser.addOption({"snapshot-probe",QStringLiteral("只读验证非空任务列表摘要和 Qt 批次控件")});
    parser.addOption({"screenshot",QStringLiteral("保存演示窗口截图；需 --demo"),"path"});
    parser.addOption({"review-dir",QStringLiteral("保存全部页面和关键弹窗的离屏截图；需 --demo"),"path"});parser.process(app);
    const bool demo=parser.isSet("demo");
    if((parser.isSet("self-test")||parser.isSet("screenshot")||parser.isSet("review-dir"))&&!demo){std::fprintf(stderr,"--self-test/--screenshot/--review-dir requires --demo\n");return 64;}
    try {
        const auto config=demo?GuiSettings{}:settings(parser);
        if(parser.isSet("probe")) {
            if(demo){std::fprintf(stderr,"--probe cannot use --demo\n");return 64;}
            RpcClient rpc;rpc.configure(config.host,config.port,config.timeout);
            auto failed=[&app](const RpcReply& r){std::fprintf(stderr,"QT_SERVER_PROBE FAIL: %s\n",r.error.toUtf8().constData());app.exit(1);};
            rpc.request("query_capabilities",Json::object(),[&](RpcReply r){
                if(!r.ok){failed(r);return;}
                rpc.request("query_workers",{{"limit",1}},[&](RpcReply r){
                    if(!r.ok){failed(r);return;}
                    rpc.request("query_tasks",{{"limit",1}},[&](RpcReply r){
                        if(!r.ok){failed(r);return;}std::fprintf(stderr,"QT_SERVER_PROBE PASS\n");app.exit(0);
                    });
                });
            });
            return app.exec();
        }
        MainWindow window(config,demo);
        window.show();
        if(parser.isSet("snapshot-probe")){
            if(demo){std::fprintf(stderr,"--snapshot-probe cannot use --demo\n");return 64;}
            QTimer poll;poll.setInterval(100);QObject::connect(&poll,&QTimer::timeout,&window,[&]{if(window.snapshotProbe())app.exit(0);});poll.start();
            QTimer::singleShot(30000,&window,[&]{std::fprintf(stderr,"QT_SNAPSHOT_PROBE FAIL\n");app.exit(1);});return app.exec();
        }
        if(parser.isSet("catalog-probe")){
            if(demo){std::fprintf(stderr,"--catalog-probe cannot use --demo\n");return 64;}
            QTimer poll;poll.setInterval(100);
            QObject::connect(&poll,&QTimer::timeout,&window,[&]{if(window.catalogProbe())app.exit(0);});poll.start();
            QTimer::singleShot(30000,&window,[&]{std::fprintf(stderr,"QT_CATALOG_PROBE FAIL: directory not ready; check Server catalog settings\n");app.exit(1);});
            return app.exec();
        }
        if(parser.isSet("self-test"))QTimer::singleShot(0,&window,[&]{const bool ok=window.selfTest();std::fprintf(stderr,"QT_GUI_SELF_TEST %s\n",ok?"PASS":"FAIL");app.exit(ok?0:1);});
        else if(parser.isSet("review-dir"))QTimer::singleShot(200,&window,[&]{const bool ok=window.saveReview(parser.value("review-dir"));app.exit(ok?0:1);});
        else if(parser.isSet("screenshot"))QTimer::singleShot(200,&window,[&]{const bool ok=window.grab().save(parser.value("screenshot"));app.exit(ok?0:1);});
        return app.exec();
    }catch(const std::exception& e){std::fprintf(stderr,"GUI: %s\nUsage: gcrp_gui --ip ADDRESS --port PORT\n",e.what());return 64;}
}
