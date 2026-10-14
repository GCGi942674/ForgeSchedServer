#include "MainWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <cstdio>

namespace {
QString findConfig() {
    const auto explicitPath=QString::fromLocal8Bit(qgetenv("GCRP_CONFIG"));
    if(qEnvironmentVariableIsSet("GCRP_CONFIG"))return explicitPath;
    for(const auto& base:{QDir::currentPath(),QCoreApplication::applicationDirPath()}){
        QDir directory(base);for(int i=0;i<8;++i){auto path=directory.filePath("config/gcrp.conf");if(QFile::exists(path))return path;if(!directory.cdUp())break;}
    }return {};
}
GuiSettings settings(const QString& path) {
    QFile file(path);if(path.isEmpty()||!file.open(QIODevice::ReadOnly))throw std::runtime_error("cannot read config/gcrp.conf; use --config");
    QTextStream stream(&file);stream.setCodec("UTF-8");QMap<QString,QString> values;
    while(!stream.atEnd()){auto line=stream.readLine().trimmed();if(line.isEmpty()||line.startsWith('#'))continue;auto at=line.indexOf('=');if(at<1)throw std::runtime_error("malformed config line");values[line.left(at).trimmed()]=line.mid(at+1).trimmed();}
    auto integer=[&values](const QString& key,int fallback,int low,int high){bool ok=false;int value=values.contains(key)?values[key].toInt(&ok):fallback;if(!values.contains(key))ok=true;if(!ok||value<low||value>high)throw std::runtime_error(("invalid "+key).toStdString());return value;};
    GuiSettings result;result.host=values.value("network.server_ip");if(result.host.isEmpty())throw std::runtime_error("network.server_ip is required");
    result.port=quint16(integer("server.port",0,1,65535));result.timeout=integer("network.timeout_ms",5000,100,120000);
    result.refreshSeconds=integer("gui.refresh_seconds",5,1,3600);result.logPageBytes=integer("gui.log_page_bytes",8192,1,16384);
    result.catalogFile=values.value("gui.catalog_file");result.target=values.value("smoke.target");return result;
}
}
int main(int argc,char** argv) {
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc,argv);app.setApplicationName("GCRPGui");app.setApplicationVersion("0.1");
    QCommandLineParser parser;parser.setApplicationDescription(QStringLiteral("GCRP C++ / Qt5 调度控制台"));
    parser.addHelpOption();parser.addVersionOption();
    parser.addOption({"demo",QStringLiteral("离线演示，不连接 Server")});
    parser.addOption({"self-test",QStringLiteral("自动检查窗口与目录树（需 --demo）")});
    parser.addOption({"config",QStringLiteral("共享配置文件"),"path"});
    parser.addOption({"probe",QStringLiteral("只读检查 Server 能力、工作机和任务接口")});
    parser.addOption({"screenshot",QStringLiteral("保存演示窗口截图；需 --demo"),"path"});parser.process(app);
    const bool demo=parser.isSet("demo");
    if((parser.isSet("self-test")||parser.isSet("screenshot"))&&!demo){std::fprintf(stderr,"--self-test/--screenshot requires --demo\n");return 64;}
    try {
        const auto config=demo?GuiSettings{}:settings(parser.isSet("config")?parser.value("config"):findConfig());
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
        if(parser.isSet("self-test"))QTimer::singleShot(0,&window,[&]{const bool ok=window.selfTest();std::fprintf(stderr,"QT_GUI_SELF_TEST %s\n",ok?"PASS":"FAIL");app.exit(ok?0:1);});
        else if(parser.isSet("screenshot"))QTimer::singleShot(200,&window,[&]{const bool ok=window.grab().save(parser.value("screenshot"));app.exit(ok?0:1);});
        return app.exec();
    }catch(const std::exception& e){std::fprintf(stderr,"GUI: %s\n",e.what());QMessageBox::critical(nullptr,QStringLiteral("GUI 配置错误"),QString::fromUtf8(e.what()));return 1;}
}
