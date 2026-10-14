#include "MainWindow.h"
#include "Theme.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QSplitter>
#include <QGroupBox>
#include <QHeaderView>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QDialog>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSignalBlocker>
#include <QTreeWidgetItemIterator>
#include <QUuid>
#include <QDateTime>
#include <algorithm>

namespace {
QString text(const Json& row, const char* key) {
    if (!row.is_object() || !row.contains(key) || row[key].is_null()) return QStringLiteral("—");
    const auto value = row[key].is_string() ? row[key].get<std::string>() : row[key].dump();
    return QString::fromStdString(value);
}
QWidget* page(const QString& title, const QString& subtitle) {
    auto widget = new QWidget;
    auto layout = new QVBoxLayout(widget);
    layout->setContentsMargins(24,20,24,20); layout->setSpacing(14);
    auto label = new QLabel(title); label->setObjectName("pageTitle");
    layout->addWidget(label);
    auto description = new QLabel(subtitle); description->setWordWrap(true);
    description->setObjectName("muted"); layout->addWidget(description);
    return widget;
}
QTableWidget* table(const QStringList& headers) {
    auto widget = new QTableWidget(0, headers.size());
    widget->setHorizontalHeaderLabels(headers);
    widget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    widget->setSelectionBehavior(QAbstractItemView::SelectRows);
    widget->setSelectionMode(QAbstractItemView::SingleSelection);
    widget->setAlternatingRowColors(true);
    widget->verticalHeader()->hide(); widget->verticalHeader()->setDefaultSectionSize(52);
    widget->setShowGrid(false); widget->setMouseTracking(true);
    widget->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    return widget;
}
void cell(QTableWidget* table, int row, int column, const QString& value) {
    auto item = new QTableWidgetItem(value);
    if(value.startsWith("ONLINE")||value=="PASS"||value==QStringLiteral("新通过"))item->setForeground(QColor("#287a61"));
    else if(value=="FAIL"||value=="FAILED"||value==QStringLiteral("新回退"))item->setForeground(QColor("#b44058"));
    else if(value=="TIMEOUT"||value=="DRAINING")item->setForeground(QColor("#987022"));
    else if(value=="OFFLINE")item->setForeground(QColor("#8795aa"));
    table->setItem(row,column,item);
}
QStringList selectedCases(QTreeWidget* tree) {
    QStringList paths;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        const auto path = (*it)->data(0,Qt::UserRole).toString();
        if (!path.isEmpty() && (*it)->checkState(0) == Qt::Checked) paths.append(path);
    }
    return paths;
}
QPushButton* pendingButton(const QString& label) {
    auto button = new QPushButton(label+QStringLiteral(" · 待接入"));
    button->setEnabled(false);
    button->setToolTip(QStringLiteral("当前 Server 没有对应 Client API"));
    return button;
}
Json exampleCatalog() {
    Json result = {{"cases",{"kintexuplus/xcku3p-ffvd900-1-i/convergentRoundingEven/run.tcl",
                      "kintexuplus/xcku3p-ffvd900-1-i/shiftRegister/run.tcl",
                      "virtexuplus/xcvu9p-flga2104-2-i/fifoSync/run.tcl"}},
                  {"revisions",{"19008","18962","18954"}}};
    result["templates"]["route"]["flow_config"] = {{"route_design",1},{"write_bitstream",1},
                                                  {"bit_cmp",1},{"msk_cmp",1},{"bgn_cmp",1}};
    return result;
}
}

MainWindow::MainWindow(GuiSettings settings, bool demo)
    : settings_(std::move(settings)), demo_(demo), rpc_(this) {
    setWindowTitle(QStringLiteral("ForgeSched 调度控制台")); resize(1280,820);
    setMinimumSize(900,600);
    auto central = new QWidget; central->setObjectName("surface"); setCentralWidget(central);
    auto shell = new QHBoxLayout(central); shell->setContentsMargins(0,0,0,0); shell->setSpacing(0);
    auto side = new QWidget; side->setObjectName("sidebar"); side->setFixedWidth(208);
    auto sideLayout = new QVBoxLayout(side); sideLayout->setContentsMargins(16,26,16,20);
    auto brand = new QLabel(QStringLiteral("ForgeSched")); brand->setObjectName("brand");
    auto brandRow = new QHBoxLayout; auto badge = new QLabel("F");
    badge->setObjectName("brandBadge"); badge->setFixedSize(32,32); badge->setAlignment(Qt::AlignCenter);
    brandRow->addWidget(badge); brandRow->addWidget(brand,1); sideLayout->addLayout(brandRow);
    auto caption = new QLabel(QStringLiteral("调度控制台")); caption->setObjectName("muted"); sideLayout->addWidget(caption);
    sideLayout->addSpacing(22); pages_ = new QStackedWidget;
    auto transition = ForgeUi::pageTransition(pages_,this);
    const QStringList labels{QStringLiteral("工作机状态"),QStringLiteral("任务管理"),
                             QStringLiteral("添加任务"),QStringLiteral("每日回归")};
    for (int i=0;i<labels.size();++i) {
        auto button = new QPushButton(labels[i]); button->setCheckable(true);
        button->setObjectName("navigation"); button->setIcon(ForgeUi::navigationIcon(i));
        button->setIconSize(QSize(20,20)); sideLayout->addWidget(button);
        connect(button,&QPushButton::clicked,this,[this,side,i,transition]{
            if(pages_->currentIndex()!=i){transition->capture();pages_->setCurrentIndex(i);transition->start();}
            for (auto b:side->findChildren<QPushButton*>()) b->setChecked(false);
            side->findChildren<QPushButton*>()[i]->setChecked(true);
        });
        if (i==0) button->setChecked(true);
    }
    sideLayout->addStretch();
    auto footer = new QLabel(demo_ ? QStringLiteral("演示模式\n示例数据 · 不发送任务") :
        settings_.host+":"+QString::number(settings_.port)+QStringLiteral("\n日志保留 30 天"));
    footer->setObjectName("sidebarFooter"); footer->setWordWrap(true); sideLayout->addWidget(footer); shell->addWidget(side);
    auto body = new QWidget; body->setObjectName("workspace"); auto bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0,0,0,0);
    auto bar = new QHBoxLayout; bar->setContentsMargins(22,12,22,5);
    connection_ = new QLabel; connection_->setTextFormat(Qt::PlainText); connection_->setWordWrap(true); bar->addWidget(connection_,1);
    auto refresh = new QPushButton(QStringLiteral("刷新")); bar->addWidget(refresh);
    connect(refresh,&QPushButton::clicked,this,[this]{this->refresh();});
    bodyLayout->addLayout(bar);
    pages_->addWidget(workersPage()); pages_->addWidget(tasksPage());
    pages_->addWidget(addPage()); pages_->addWidget(regressionPage());
    bodyLayout->addWidget(pages_,1); shell->addWidget(body,1);
    setStyleSheet(ForgeUi::styleSheet());
    for(auto button:findChildren<QPushButton*>())button->setCursor(button->isEnabled()?Qt::PointingHandCursor:Qt::ArrowCursor);
    if (demo_) {
        for (const auto& machine: {"novarch4","novarch3","tiger"})
            for (int i=1;i<=(std::string(machine)=="novarch4"?5:3);++i)
                workers_.push_back({{"machine_id",machine},{"worker_id",std::string(machine)+"-slot"+std::to_string(i)},
                    {"slot_id","slot"+std::to_string(i)},{"status",std::string(machine)=="tiger"?"OFFLINE":"ONLINE"},
                    {"used",i==1?1:0},{"capacity",1},{"free",i==1?0:1}});
        tasks_={{{"task_id",1042},{"status","RUNNING"},{"target","xcku3p-ffvd900-1-i"},
                 {"revision","19008"},{"worker_id","novarch4-slot1"},{"batch_id","gui-demo"},{"run_id","demo-run"}},
                {{"task_id",1043},{"status","QUEUED"},{"target","xcku3p-ffvd900-1-i"},
                 {"revision","19008"},{"worker_id",""},{"batch_id","gui-demo"},{"run_id",""}}};
        catalog_=exampleCatalog(); loadCatalog(); showWorkers(); machines_->setCurrentText("novarch4"); showTasks();
        note(QStringLiteral("演示模式：所有数据和操作均为本地示例"));
    } else {
        rpc_.configure(settings_.host,settings_.port,settings_.timeout); loadCatalog();
        rpc_.request("query_capabilities",Json::object(),[this](RpcReply r){
            if (!r.ok) { note(r.error,true); return; }
            for (const auto& op:r.result.value("operations",Json::array()))
                if (op.is_string()) operations_.insert(QString::fromStdString(op.get<std::string>()));
            submit_->setEnabled(!catalog_.is_null() && operations_.contains("submit_task"));
            this->refresh();
        });
        refreshTimer_.setInterval(settings_.refreshSeconds*1000);
        connect(&refreshTimer_,&QTimer::timeout,this,[this]{this->refresh();}); refreshTimer_.start();
        note(QStringLiteral("正在连接 Server…"));
    }
}
QWidget* MainWindow::workersPage() {
    auto widget=page(QStringLiteral("工作机状态"),QStringLiteral("按主机查看子 slots；ONLINE 与忙碌/空闲分开显示。"));
    auto layout=qobject_cast<QVBoxLayout*>(widget->layout());
    auto bar=new QHBoxLayout; machines_=new QComboBox; bar->addWidget(machines_,1);
    bar->addWidget(pendingButton(QStringLiteral("整机上线"))); bar->addWidget(pendingButton(QStringLiteral("整机下线")));
    layout->addLayout(bar); slotsTable_=table({"slot","Worker ID","连接状态","占用 / 容量","当前任务"});
    layout->addWidget(ForgeUi::card(slotsTable_),1);
    auto hint=new QLabel(QStringLiteral("主机列表来自自动注册信息。远程上下线尚未接入；下线应允许当前 testcase 跑完。"));
    hint->setWordWrap(true); layout->addWidget(hint);
    connect(machines_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{showWorkers();});
    connect(slotsTable_,&QTableWidget::cellDoubleClicked,this,[this](int row,int){
        if (auto item=slotsTable_->item(row,4)) {
            bool ok=false; auto id=item->data(Qt::UserRole).toString().toULongLong(&ok);
            if (ok) openTask(id);
        }
    }); return widget;
}
QWidget* MainWindow::tasksPage() {
    auto widget=page(QStringLiteral("任务管理"),QStringLiteral("按 batch_id 分组；双击任务查看参数和成功、失败的归档日志。"));
    auto layout=qobject_cast<QVBoxLayout*>(widget->layout());
    auto actions=new QHBoxLayout;
    auto cancel=new QPushButton(QStringLiteral("取消未执行任务 / 批次")); cancel->setProperty("kind","danger"); actions->addWidget(cancel);
    actions->addWidget(pendingButton(QStringLiteral("删除记录"))); actions->addStretch();
    auto details=new QPushButton(QStringLiteral("查看任务 / 日志"));details->setProperty("kind","primary"); actions->addWidget(details);layout->addLayout(actions);
    tasksTree_=new QTreeWidget;
    tasksTree_->setHeaderLabels({"批次 / Task","状态","目标","版本","工作机"});
    tasksTree_->setRootIsDecorated(true);tasksTree_->setAlternatingRowColors(true);tasksTree_->setMouseTracking(true);tasksTree_->setIndentation(24);
    tasksTree_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tasksTree_->header()->setStretchLastSection(true);layout->addWidget(ForgeUi::card(tasksTree_),1);
    connect(cancel,&QPushButton::clicked,this,[this]{cancelSelection();});
    auto open=[this]{auto item=tasksTree_->currentItem();if(item&&item->data(0,Qt::UserRole).isValid())openTask(item->data(0,Qt::UserRole).toString().toULongLong());};
    connect(details,&QPushButton::clicked,this,open);
    connect(tasksTree_,&QTreeWidget::itemDoubleClicked,this,[open](QTreeWidgetItem*,int){open();});return widget;
}
QWidget* MainWindow::addPage() {
    auto widget=page(QStringLiteral("添加任务"),QStringLiteral("目录树选择 testcase；人工批次不会冒充每日定时回归。"));
    auto layout=qobject_cast<QVBoxLayout*>(widget->layout());auto split=new QSplitter;
    casesTree_=new QTreeWidget;casesTree_->setHeaderLabel(QStringLiteral("Testcase 目录树"));casesTree_->setMouseTracking(true); split->addWidget(ForgeUi::card(casesTree_));
    auto formWidget=new QGroupBox(QStringLiteral("批次设置"));auto form=new QFormLayout(formWidget);
    batchName_=new QLineEdit(QStringLiteral("人工验证批次"));target_=new QLineEdit(settings_.target);
    revision_=new QComboBox;flow_=new QComboBox;
    priority_=new QSpinBox;priority_->setRange(-100,100);
    timeout_=new QSpinBox;timeout_->setRange(1,86400);timeout_->setValue(5400);
    form->addRow(QStringLiteral("批次名称"),batchName_);form->addRow(QStringLiteral("业务目标（明确填写）"),target_);
    form->addRow(QStringLiteral("GalaxCore ZIP 版本"),revision_);form->addRow(QStringLiteral("Server flow 模板"),flow_);
    form->addRow(QStringLiteral("优先级"),priority_);form->addRow(QStringLiteral("单 case 超时 / 秒"),timeout_);
    auto snapshot=new QPushButton(QStringLiteral("查看 flow_config 快照"));form->addRow(snapshot);
    auto importButton=new QPushButton(QStringLiteral("导入服务端目录清单"));form->addRow(importButton);
    auto hint=new QLabel(QStringLiteral("当前 Server 没有目录清单 API。先从 Server 导出清单并导入；提交时 Server 仍校验参数。"));
    hint->setWordWrap(true);form->addRow(hint);split->addWidget(formWidget);split->setStretchFactor(0,3);split->setStretchFactor(1,2);
    layout->addWidget(split,1);auto bottom=new QHBoxLayout;selection_=new QLabel(QStringLiteral("已选 0 个 testcase"));bottom->addWidget(selection_,1);
    submit_=new QPushButton(QStringLiteral("预览并提交"));submit_->setProperty("kind","primary"); submit_->setEnabled(demo_);bottom->addWidget(submit_);layout->addLayout(bottom);
    connect(casesTree_,&QTreeWidget::itemChanged,this,[this]{selection_->setText(QStringLiteral("已选 %1 个 testcase").arg(selectedCases(casesTree_).size()));});
    connect(importButton,&QPushButton::clicked,this,[this]{auto path=QFileDialog::getOpenFileName(this,QStringLiteral("导入 Server 目录清单"),{},"JSON (*.json)");if(!path.isEmpty()){settings_.catalogFile=path;loadCatalog();}});
    connect(snapshot,&QPushButton::clicked,this,[this]{const auto key=flow_->currentText().toStdString();if(catalog_.is_object()&&catalog_.contains("templates")&&catalog_["templates"].contains(key))QMessageBox::information(this,QStringLiteral("目录快照；Server 提交时校验"),QString::fromStdString(catalog_["templates"][key].dump(2)));});
    connect(submit_,&QPushButton::clicked,this,[this]{submitBatch();});return widget;
}
QWidget* MainWindow::regressionPage() {
    auto widget=page(QStringLiteral("每日回归"),QStringLiteral("按日期比较每日定时批次；人工批次独立，缺少结果单独列出。"));
    auto layout=qobject_cast<QVBoxLayout*>(widget->layout());auto bar=new QHBoxLayout;
    dateA_=new QDateEdit(QDate(2026,10,11));dateB_=new QDateEdit(QDate(2026,10,12));
    dateA_->setCalendarPopup(true);dateB_->setCalendarPopup(true);dateA_->setDisplayFormat("yyyy-MM-dd");dateB_->setDisplayFormat("yyyy-MM-dd");
    bar->addWidget(new QLabel(QStringLiteral("基准日期")));bar->addWidget(dateA_);bar->addWidget(new QLabel(QStringLiteral("比较日期")));bar->addWidget(dateB_);
    auto compare=new QPushButton(QStringLiteral("比较"));compare->setEnabled(demo_);bar->addWidget(compare);bar->addStretch();layout->addLayout(bar);
    regressionTable_=table({"testcase","基准结果","比较结果","变化"});layout->addWidget(ForgeUi::card(regressionTable_),1);
    auto hint=new QLabel(demo_?QStringLiteral("演示数据：PASS→FAIL 为新回退；TIMEOUT 和缺失结果另外展示。"):
        QStringLiteral("待接入：每日回归来源 / 日期查询 API。现有 query_rollback 是按版本比较，不用于替代这里的日期比较。"));
    hint->setWordWrap(true);layout->addWidget(hint);connect(compare,&QPushButton::clicked,this,[this]{compareDemoDates();});if(demo_)compareDemoDates();return widget;
}
void MainWindow::note(const QString& text, bool error) {
    connection_->setText(text);connection_->setStyleSheet(error?"color:#ad3442":"color:#52647b");
}
void MainWindow::fetchList(const std::string& type,const Json& filter,const Json& cursor,
    std::shared_ptr<Json> rows,std::string instance,std::function<void(bool,Json,QString)> done,int page) {
    Json data=filter;data["limit"]=50;if(!cursor.is_null())data[type=="query_workers"?"after":"before_task_id"]=cursor;
    rpc_.request(type,data,[this,type,filter,rows,instance,done,page](RpcReply r){
        if(!r.ok){done(false,{},r.error);return;}
        if(!instance.empty()&&instance!=r.instance){done(false,{},QStringLiteral("Server 已重启，请重新刷新；保留原数据"));return;}
        try {
            const auto& items=r.result.at("items");if(!items.is_array())throw std::runtime_error("items must be array");
            for(const auto& row:items)rows->push_back(row);
            if(r.result.value("has_more",false)){
                if(page>=19){done(true,*rows,QStringLiteral("仅加载前 1000 条；未加载条目未计入当前视图"));return;}
                auto next=r.result.at(type=="query_workers"?"next_after":"next_before_task_id");
                if(next.is_null()||items.empty())throw std::runtime_error("invalid cursor");
                fetchList(type,filter,next,rows,r.instance,done,page+1);
            }else done(true,*rows,{});
        }catch(const std::exception& e){done(false,{},QString::fromUtf8(e.what()));}
    });
}
void MainWindow::refresh() {
    if(demo_){showWorkers();showTasks();return;}if(loading_||submitting_)return;
    loading_=true;
    fetchList("query_workers",Json::object(),nullptr,std::make_shared<Json>(Json::array()),{},
      [this](bool ok,Json rows,QString warning){
        if(!ok){loading_=false;note(QStringLiteral("数据未更新：")+warning,true);return;}
        workers_=std::move(rows);showWorkers();
        fetchList("query_tasks",Json::object(),nullptr,std::make_shared<Json>(Json::array()),{},
          [this,warning](bool success,Json rows,QString taskWarning){loading_=false;if(!success){note(QStringLiteral("任务数据未更新：")+taskWarning,true);return;}tasks_=std::move(rows);showTasks();note(QStringLiteral("已连接 · 更新于 ")+QDateTime::currentDateTime().toString("HH:mm:ss")+" "+warning+taskWarning);});
    });
}
void MainWindow::showWorkers() {
    const auto selected=machines_->currentText();QSet<QString> machineNames;
    for(const auto& w:workers_)machineNames.insert(text(w,"machine_id"));
    {QSignalBlocker blocked(machines_);machines_->clear();auto list=machineNames.values();list.sort();machines_->addItems(list);if(list.contains(selected))machines_->setCurrentText(selected);}
    slotsTable_->setRowCount(0);
    for(const auto& w:workers_)if(text(w,"machine_id")==machines_->currentText()){
        int row=slotsTable_->rowCount();slotsTable_->insertRow(row);cell(slotsTable_,row,0,text(w,"slot_id"));cell(slotsTable_,row,1,text(w,"worker_id"));
        auto status=text(w,"status");if(status=="ONLINE")status+=w.value("used",0)>0?QStringLiteral(" · 忙碌"):QStringLiteral(" · 空闲");
        cell(slotsTable_,row,2,status);cell(slotsTable_,row,3,text(w,"used")+" / "+text(w,"capacity"));
        cell(slotsTable_,row,4,"—");for(const auto& t:tasks_)if(text(t,"worker_id")==text(w,"worker_id")&&(text(t,"status")=="RUNNING"||text(t,"status")=="ASSIGNED")){
            cell(slotsTable_,row,4,"Task #"+text(t,"task_id"));slotsTable_->item(row,4)->setData(Qt::UserRole,text(t,"task_id"));break;
        }
    }
}
void MainWindow::showTasks() {
    const auto current=tasksTree_->currentItem();QString selected=current?current->data(0,Qt::UserRole).toString():QString{};
    tasksTree_->clear();QMap<QString,QTreeWidgetItem*> batches;
    for(const auto& t:tasks_){auto batch=text(t,"batch_id");if(batch.isEmpty()||batch=="—")batch=QStringLiteral("无批次 / 单任务");
        auto parent=batches.value(batch,nullptr);if(!parent){parent=new QTreeWidgetItem(tasksTree_,{batch});parent->setData(0,Qt::UserRole+1,batch);parent->setExpanded(true);batches[batch]=parent;}
        auto item=new QTreeWidgetItem(parent,{"Task #"+text(t,"task_id"),text(t,"status"),text(t,"target"),text(t,"revision"),text(t,"worker_id")});
        item->setData(0,Qt::UserRole,text(t,"task_id"));if(item->data(0,Qt::UserRole).toString()==selected)tasksTree_->setCurrentItem(item);
    }showWorkers();
}
void MainWindow::loadCatalog() {
    try {
        if(!demo_){if(settings_.catalogFile.isEmpty()){submit_->setEnabled(false);return;}QFile file(settings_.catalogFile);if(!file.open(QIODevice::ReadOnly)||file.size()>16*1024*1024)throw std::runtime_error("cannot read catalog (maximum 16 MiB)");auto bytes=file.readAll();catalog_=Json::parse(bytes.constData(),bytes.constData()+bytes.size());}
        if(!catalog_.at("cases").is_array()||catalog_["cases"].size()>50000||!catalog_.at("templates").is_object()||!catalog_.at("revisions").is_array())throw std::runtime_error("invalid catalog");
        for(const auto& version:catalog_["revisions"]){if(!version.is_string())throw std::runtime_error("revision must be text");const auto v=version.get<std::string>();if(v.empty()||v.size()>32||v.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("revision must be 1-32 decimal digits");}
        for(auto it=catalog_["templates"].begin();it!=catalog_["templates"].end();++it){if(!it.value().is_object()||!it.value().contains("flow_config")||!it.value()["flow_config"].is_object()||it.value()["flow_config"].empty())throw std::runtime_error("template requires nonempty flow_config");}
        for(const auto& value:catalog_["cases"]){if(!value.is_string())throw std::runtime_error("invalid case path");auto path=QString::fromStdString(value.get<std::string>());auto parts=path.split('/');if(path.startsWith('/')||path.contains('\\')||path.contains(':')||parts.contains("..")||parts.contains(".")||parts.contains("")||!path.endsWith("/run.tcl"))throw std::runtime_error("case must be a safe relative run.tcl path");}
        casesTree_->clear();revision_->clear();flow_->clear();auto root=new QTreeWidgetItem(casesTree_,{QStringLiteral("test2")});root->setFlags(root->flags()|Qt::ItemIsUserCheckable|Qt::ItemIsAutoTristate);root->setCheckState(0,Qt::Unchecked);
        QMap<QString,QTreeWidgetItem*> directories;directories[""]=root;
        for(const auto& value:catalog_["cases"]){auto path=QString::fromStdString(value.get<std::string>());auto parts=path.split('/');QString key;auto parent=root;
            for(int i=0;i<parts.size()-1;++i){key+=(key.isEmpty()?"":"/")+parts[i];auto item=directories.value(key,nullptr);if(!item){item=new QTreeWidgetItem(parent,{parts[i]});item->setFlags(item->flags()|Qt::ItemIsUserCheckable|Qt::ItemIsAutoTristate);item->setCheckState(0,Qt::Unchecked);directories[key]=item;}parent=item;}
            auto leaf=new QTreeWidgetItem(parent,{"run.tcl"});leaf->setFlags(leaf->flags()|Qt::ItemIsUserCheckable);leaf->setCheckState(0,Qt::Unchecked);leaf->setData(0,Qt::UserRole,path);leaf->setToolTip(0,path);
        }casesTree_->expandToDepth(2);
        for(const auto& r:catalog_["revisions"])revision_->addItem(QString::fromStdString(r.get<std::string>()));
        for(auto it=catalog_["templates"].begin();it!=catalog_["templates"].end();++it)flow_->addItem(QString::fromStdString(it.key()));
        submit_->setEnabled(demo_||operations_.contains("submit_task"));
    }catch(const std::exception& e){catalog_=nullptr;submit_->setEnabled(false);note(QStringLiteral("目录清单错误：")+QString::fromUtf8(e.what()),true);}
}
void MainWindow::submitBatch() {
    if(submitting_)return;auto paths=selectedCases(casesTree_);
    if(paths.isEmpty()||batchName_->text().trimmed().isEmpty()||(!demo_&&target_->text().trimmed().isEmpty())||revision_->currentText().isEmpty()||flow_->currentText().isEmpty()){
        QMessageBox::warning(this,QStringLiteral("参数不完整"),QStringLiteral("选择 testcase，并填写名称、业务目标、版本和 flow。"));return;}
    if(QMessageBox::question(this,QStringLiteral("确认人工批次"),QStringLiteral("%1\n%2 个 testcase · r%3 · %4\n取消只处理未开始的 testcase。\n逐个提交，失败会保留已接收的任务，不自动重试。").arg(batchName_->text()).arg(paths.size()).arg(revision_->currentText()).arg(flow_->currentText()))!=QMessageBox::Yes)return;
    // The default UUID representation is {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}.
    // Remove its braces explicitly for compatibility with older Qt5 headers.
    manualBatch_="gui-"+QUuid::createUuid().toString().mid(1,36);pendingSubmissions_=Json::array();submitIndex_=0;
    for(const auto& path:paths){pendingSubmissions_.push_back({{"task_type","REGRESSION"},{"target",target_->text().toStdString()},{"revision",revision_->currentText().toStdString()},{"priority",priority_->value()},
        {"submission_key",(manualBatch_+"-"+QString::number(pendingSubmissions_.size())).toStdString()},
        {"payload",{{"spec_version",2},{"case",path.toStdString()},{"flow",flow_->currentText().toStdString()},{"flow_config",catalog_["templates"][flow_->currentText().toStdString()].at("flow_config")},{"timeout_seconds",timeout_->value()},
        {"context",{{"batch_id",manualBatch_.toStdString()},{"name",batchName_->text().toStdString()},{"suite","manual"}}}}}});}
    if(demo_){note(QStringLiteral("演示：批次预览通过；未发送任何网络请求"));return;}
    submitting_=true;submit_->setEnabled(false);submitNext();
}
void MainWindow::submitNext() {
    if(submitIndex_==int(pendingSubmissions_.size())){submitting_=false;submit_->setEnabled(true);QMessageBox::information(this,QStringLiteral("提交完成"),QStringLiteral("已接收 %1 个任务\n批次 %2").arg(submitIndex_).arg(manualBatch_));refresh();return;}
    note(QStringLiteral("提交批次 %1：%2 / %3 已接收").arg(manualBatch_).arg(submitIndex_).arg(pendingSubmissions_.size()));
    rpc_.request("submit_task",pendingSubmissions_[submitIndex_],[this](RpcReply r){
        if(!r.ok){submitting_=false;submit_->setEnabled(true);QMessageBox::warning(this,QStringLiteral("部分提交 / 结果可能不确定"),QStringLiteral("已确认接收 %1 个任务。\n批次：%2\n%3\n请先查询本批次；直接重新提交会创建新批次。").arg(submitIndex_).arg(manualBatch_).arg(r.error));refresh();return;}++submitIndex_;submitNext();
    });
}
void MainWindow::cancelSelection() {
    auto item=tasksTree_->currentItem();if(!item)return;
    const bool one=item->data(0,Qt::UserRole).isValid();auto id=item->data(0,Qt::UserRole).toString();auto batch=item->data(0,Qt::UserRole+1).toString();
    if(!one&&batch==QStringLiteral("无批次 / 单任务"))return;
    if(QMessageBox::question(this,QStringLiteral("确认取消"),QStringLiteral("仅取消未开始的 testcase；ASSIGNED/RUNNING 不受影响。取消记录保留。"))!=QMessageBox::Yes)return;
    if(demo_){for(auto& t:tasks_)if(text(t,"status")=="QUEUED"&&(one?text(t,"task_id")==id:text(t,"batch_id")==batch))t["status"]="CANCELLED";showTasks();return;}
    auto op=one?"cancel_task":"cancel_batch";
    if(!operations_.contains(op)){note(QStringLiteral("Server 未声明此操作"),true);return;}
    rpc_.request(op,one?Json{{"task_id",id.toULongLong()}}:Json{{"batch_id",batch.toStdString()}},[this](RpcReply r){if(!r.ok)QMessageBox::warning(this,QStringLiteral("取消未完成"),r.error);refresh();});
}
void MainWindow::openTask(uint64_t id) {
    auto display=[this](RpcReply r){if(!r.ok){QMessageBox::warning(this,QStringLiteral("查询失败"),r.error);return;}
        auto dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(QStringLiteral("Task 详情与日志"));dialog->resize(850,650);auto layout=new QVBoxLayout(dialog);
        auto body=new QPlainTextEdit(QString::fromStdString(r.result.dump(2)));body->setReadOnly(true);layout->addWidget(body,1);
        auto logs=new QPushButton(QStringLiteral("查看 Server 归档日志"));layout->addWidget(logs);connect(logs,&QPushButton::clicked,this,[this,task=r.result]{openLogs(task);});dialog->show();};
    if(demo_){Json t={{"task_id",id},{"run_id","demo-run"},{"status","SUCCEEDED"},{"payload",{{"case","convergentRoundingEven/run.tcl"},{"flow","route"}}}};display({true,{},t,{}});}
    else rpc_.request("query_task",{{"task_id",id}},display);
}
void MainWindow::openLogs(const Json& task) {
    if(!task.contains("run_id")||text(task,"run_id").isEmpty()||text(task,"status")=="RUNNING"||text(task,"status")=="ASSIGNED"){QMessageBox::information(this,QStringLiteral("日志待生成 / 待上传"),QStringLiteral("当前 API 支持结束后的归档日志。缺少日志不代表执行失败。"));return;}
    auto dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(QStringLiteral("执行日志 · 成功与失败均可查看"));dialog->resize(950,650);
    auto layout=new QVBoxLayout(dialog);auto controls=new QHBoxLayout;auto files=new QComboBox;controls->addWidget(files,1);auto next=new QPushButton(QStringLiteral("下一段"));controls->addWidget(next);next->setEnabled(false);layout->addLayout(controls);
    auto status=new QLabel(QStringLiteral("读取当前 run_id 的日志文件清单…"));status->setTextFormat(Qt::PlainText);status->setWordWrap(true);layout->addWidget(status);
    auto body=new QPlainTextEdit;body->setReadOnly(true);body->setFont(QFont("monospace",10));layout->addWidget(body,1);
    auto search=new QLineEdit;search->setPlaceholderText(QStringLiteral("只搜索当前已加载文本"));layout->addWidget(search);connect(search,&QLineEdit::returnPressed,body,[body,search]{body->find(search->text());});
    auto offset=std::make_shared<uint64_t>(0);auto reading=std::make_shared<bool>(false);QPointer<QDialog> guard(dialog);
    auto read=std::make_shared<std::function<void()>>();
    *read=[this,guard,task,files,next,status,body,offset,reading]{if(!guard||*reading||files->currentText().isEmpty())return;if(demo_){body->setPlainText("[RUNNER] convergentRoundingEven\n[COMPARE] bit_cmp PASS\n[RESULT] COMPARE_ARTIFACTS_PASS\n");status->setText(QStringLiteral("示例日志；真实模式按偏移读取 Server 归档"));return;}
        *reading=true;files->setEnabled(false);next->setEnabled(false);auto name=files->currentData().toString();auto requestOffset=*offset;
        rpc_.request("read_log",{{"task_id",task.at("task_id")},{"run_id",task.at("run_id")},{"name",name.toStdString()},{"offset",requestOffset},{"limit",settings_.logPageBytes}},[guard,files,next,status,body,offset,reading,name,requestOffset](RpcReply r){if(!guard)return;*reading=false;files->setEnabled(true);if(files->currentData().toString()!=name){status->setText(QStringLiteral("文件已切换，请重新选择以加载"));return;}if(!r.ok){status->setText(r.error);return;}try{
            auto hex=r.result.at("data_hex").get<std::string>();auto bytes=QByteArray::fromHex(QByteArray(hex.data(),int(hex.size())));auto size=r.result.at("size").get<uint64_t>();*offset=r.result.at("next_offset").get<uint64_t>();
            body->setPlainText(QString::fromUtf8(bytes));status->setText(QStringLiteral("当前片段 [%1, %2) / %3 字节 · %4 · UTF-8 预览，可能替换非法字节").arg(qulonglong(requestOffset)).arg(qulonglong(*offset)).arg(qulonglong(size)).arg(r.result.value("complete",false)?QStringLiteral("上传完成"):QStringLiteral("上传中")));next->setEnabled(*offset<size);
        }catch(const std::exception& e){status->setText(QString::fromUtf8(e.what()));}});
    };
    connect(next,&QPushButton::clicked,dialog,[read]{(*read)();});
    connect(files,QOverload<int>::of(&QComboBox::currentIndexChanged),dialog,[offset,reading,read,body]{*offset=0;body->clear();if(!*reading)(*read)();});
    dialog->show();
    if(demo_){files->addItem("case-run.log","case-run.log");return;}
    rpc_.request("query_logs",{{"task_id",task.at("task_id")},{"run_id",task.at("run_id")}},[guard,files,status](RpcReply r){if(!guard)return;if(!r.ok){status->setText(r.error);return;}try{const auto& items=r.result.at("items");for(const auto& f:items)files->addItem(text(f,"name")+(f.value("complete",false)?"":" · 上传中"),text(f,"name"));if(items.empty())status->setText(QStringLiteral("暂无归档文件：可能待上传、未采集或已超过 30 天；当前 API 尚不能区分这些原因"));}catch(const std::exception& e){status->setText(QString::fromUtf8(e.what()));}});
}
void MainWindow::compareDemoDates() {
    regressionTable_->setRowCount(0);if(dateA_->date()==dateB_->date())return;
    const QStringList caseNames{"convergentRoundingEven","shiftRegister","ramBlock","uartTx","dspMultiply"};
    const QStringList a{"PASS","PASS","FAIL","PASS","PASS"},b{"PASS","FAIL","PASS","TIMEOUT","未运行"};
    bool forward=dateA_->date()<dateB_->date();
    for(int i=0;i<caseNames.size();++i){auto x=forward?a[i]:b[i],y=forward?b[i]:a[i];QString change=x=="PASS"&&y=="FAIL"?QStringLiteral("新回退"):x=="FAIL"&&y=="PASS"?QStringLiteral("新通过"):y=="未运行"?QStringLiteral("缺少结果"):y=="TIMEOUT"?QStringLiteral("超时异常"):QStringLiteral("无确认回退");regressionTable_->insertRow(i);cell(regressionTable_,i,0,caseNames[i]);cell(regressionTable_,i,1,x);cell(regressionTable_,i,2,y);cell(regressionTable_,i,3,change);}
}
bool MainWindow::selfTest() {
    if(!demo_||pages_->count()!=4||machines_->count()!=3||tasksTree_->topLevelItemCount()!=1)return false;
    auto root=casesTree_->topLevelItem(0);if(!root)return false;root->setCheckState(0,Qt::Checked);
    if(selectedCases(casesTree_).size()!=3)return false;
    for(QTreeWidgetItemIterator it(casesTree_);*it;++it)if(!(*it)->data(0,Qt::UserRole).toString().isEmpty()){(*it)->setCheckState(0,Qt::Unchecked);break;}
    if(root->checkState(0)!=Qt::PartiallyChecked||selectedCases(casesTree_).size()!=2)return false;
    const auto navigation=findChildren<QPushButton*>("navigation");
    if(navigation.size()!=4)return false;
    navigation[2]->click();if(pages_->currentIndex()!=2)return false;
    navigation[0]->click();if(pages_->currentIndex()!=0)return false;
    return regressionTable_->rowCount()==5;
}
