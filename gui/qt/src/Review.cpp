#include "ConsoleSupport.h"
#include "CaseListImport.h"

void MainWindow::compareDemoDates() {
    regressionTable_->setRowCount(0);if(dateA_->date()==dateB_->date())return;
    const QStringList caseNames{"convergentRoundingEven","shiftRegister","ramBlock","uartTx","dspMultiply"};
    const QStringList a{"PASS","PASS","FAIL","PASS","PASS"},b{"PASS","FAIL","PASS","TIMEOUT","未运行"};
    bool forward=dateA_->date()<dateB_->date();
    for(int i=0;i<caseNames.size();++i){auto x=forward?a[i]:b[i],y=forward?b[i]:a[i];QString change=x=="PASS"&&y=="FAIL"?QStringLiteral("新回退"):x=="FAIL"&&y=="PASS"?QStringLiteral("新通过"):y=="未运行"?QStringLiteral("缺少结果"):y=="TIMEOUT"?QStringLiteral("超时异常"):QStringLiteral("无确认回退");regressionTable_->insertRow(i);cell(regressionTable_,i,0,"route_design");cell(regressionTable_,i,1,caseNames[i]+"/run.tcl");regressionTable_->setCellWidget(i,2,GcrpUi::badge(x));regressionTable_->setCellWidget(i,3,GcrpUi::badge(y));cell(regressionTable_,i,4,change);
        auto button=new QPushButton(QStringLiteral("查看原始 run"));regressionTable_->setCellWidget(i,5,button);connect(button,&QPushButton::clicked,this,[this,i]{openLogs(tasks_[i]);});
    }
    regressionSummary_->setText(QStringLiteral("示例：5 个 testcase · 新回退 1 · 新通过 1 · 异常 1 · 缺少 1；人工批次不参与每日对比。"));

}

bool MainWindow::selfTest() {
    // Even an older Server advertising controls must not re-enable admin UI.
    const auto previousOperations=operations_;const bool previousDemo=demo_;
    demo_=false;operations_.insert("control_workers");showWorkers();
    for(auto button:hostControlButtons_)if(button->isEnabled())return false;
    for(int row=0;row<slotsTable_->rowCount();++row)if(auto controls=slotsTable_->cellWidget(row,4))
        for(auto button:controls->findChildren<QPushButton*>())if(button->isEnabled())return false;
    demo_=previousDemo;operations_=previousOperations;showWorkers();
    const auto firstCase=QString::fromStdString(catalog_["cases"][0].get<std::string>());
    if(importCaseList(casesTree_,QByteArray("\xef\xbb\xbf")+firstCase.toUtf8()+"\r\n\n"+firstCase.toUtf8())!=1||selectedCases(casesTree_).size()!=1)return false;
    auto selectedBefore=selectedCases(casesTree_);
    for(const auto& invalid:QList<QByteArray>{QByteArray(),QByteArray("\xff"),firstCase.toUtf8()+"\nmissing/run.tcl",QByteArray("../run.tcl"),QByteArray("{}")}){
        bool rejected=false;try{importCaseList(casesTree_,invalid);}catch(const std::exception&){rejected=true;}
        if(!rejected||selectedCases(casesTree_)!=selectedBefore)return false;
    }
    auto windowsCase=firstCase;windowsCase.replace('/','\\');
    if(importCaseList(casesTree_,("./"+windowsCase+"\r").toUtf8())!=1)return false;
    if(csvCell("=2+2")!="\"'=2+2\""||csvCell("a\"b")!="\"a\"\"b\"")return false;
    if(!demo_||flowOptions_->topLevelItemCount()<19||pages_->count()!=4||taskMetrics_.size()!=4)return false;
    const auto key=flow_->currentText().toStdString();const auto defaults=catalog_["templates"][key],before=selectedFlowConfig();
    auto option=flowOptions_->topLevelItem(0);option->setCheckState(0,option->checkState(0)==Qt::Checked?Qt::Unchecked:Qt::Checked);
    if(before==selectedFlowConfig()||defaults!=catalog_["templates"][key])return false;
    resetFlowOptions();if(before!=selectedFlowConfig())return false;
    if(tasksTree_->columnCount()!=8||tasksTree_->topLevelItemCount()!=7||hostsList_->count()!=3)return false;
    taskSearch_->setText("not-an-existing-case");if(!tasksTree_->topLevelItem(0)->isHidden())return false;taskSearch_->clear();
    taskSource_->setCurrentIndex(taskSource_->findData("manual"));int count=0;for(int i=0;i<tasksTree_->topLevelItemCount();++i)if(!tasksTree_->topLevelItem(i)->isHidden())++count;if(count!=1)return false;taskSource_->setCurrentIndex(0);
    taskStatus_->setCurrentIndex(taskStatus_->findData("QUEUED"));count=0;for(int i=0;i<tasksTree_->topLevelItemCount();++i)if(!tasksTree_->topLevelItem(i)->isHidden())++count;if(count!=1)return false;taskStatus_->setCurrentIndex(0);
    auto root=casesTree_->topLevelItem(0);if(!root)return false;root->setCheckState(0,Qt::Checked);if(selectedCases(casesTree_).size()!=int(catalog_["cases"].size()))return false;
    for(QTreeWidgetItemIterator it(casesTree_);*it;++it)if(!(*it)->data(0,Qt::UserRole).toString().isEmpty()){(*it)->setCheckState(0,Qt::Unchecked);break;}
    if(root->checkState(0)!=Qt::PartiallyChecked)return false;
    const auto chosen=selectedCases(casesTree_);caseSearch_->setText("a-case-that-does-not-exist");if(!root->isHidden()||selectedCases(casesTree_)!=chosen)return false;caseSearch_->clear();
    const auto navigation=findChildren<QPushButton*>("navigation");if(navigation.size()!=4)return false;
    navigation[2]->click();if(pages_->currentIndex()!=2)return false;navigation[0]->click();if(pages_->currentIndex()!=0)return false;
    demo_=false;workersStale_=false;tasksStale_=false;workerUpdated_.start();taskUpdated_.start();invalidateSnapshots(true,true);
    if(!workersStale_||!tasksStale_||!freshness_->isVisible()||slotsTable_->rowCount()==0||!slotsTable_->item(0,1)->text().contains(QStringLiteral("数据过期")))return false;
    workersStale_=false;tasksStale_=false;updateFreshness();showWorkers();if(freshness_->isVisible())return false;demo_=true;
    machines_->setCurrentText("novarch4");showWorkers();if(slotsTable_->rowCount()!=1)return false;
    const auto original=workers_;auto offline=workers_[0];offline["worker_id"]="novarch4-slot9";offline["slot_id"]="slot9";offline["status"]="OFFLINE";offline["used"]=0;workers_.push_back(offline);
    showOffline_->setChecked(false);showWorkers();if(slotsTable_->rowCount()!=1)return false;showOffline_->setChecked(true);if(slotsTable_->rowCount()!=2)return false;workers_=original;showWorkers();
    openBatch("demo-batch-1");auto batches=findChildren<QDialog*>("batchDialog");if(batches.isEmpty())return false;auto table=batches.last()->findChild<QTableWidget*>("caseResults");if(!table||table->rowCount()!=10)return false;
    auto filter=batches.last()->findChild<QComboBox*>();filter->setCurrentIndex(filter->findData("FAIL"));bool exportOk=false;
    for(auto button:batches.last()->findChildren<QPushButton*>())if(button->text()==QStringLiteral("导出匹配清单…")){
        QTimer::singleShot(0,this,[this,&exportOk]{const auto dialogs=findChildren<QDialog*>("exportDialog");if(!dialogs.isEmpty()){auto label=dialogs.last()->findChild<QLabel*>("exportCount");auto preview=dialogs.last()->findChild<QPlainTextEdit*>("exportPreview");exportOk=label&&preview&&label->text().contains("12")&&preview->toPlainText().split('\n').size()==12;dialogs.last()->reject();}});button->click();break;
    }if(!exportOk)return false;batches.last()->close();
    openLogs(tasks_[0]);auto logs=findChildren<QDialog*>("logDialog");if(logs.isEmpty())return false;
    auto files=logs.last()->findChild<QListWidget*>("logFiles");auto body=logs.last()->findChild<QPlainTextEdit*>("logBody");
    if(!files||files->count()!=6||!body||!body->toPlainText().contains(QStringLiteral("[PHASE] route_design")))return false;
    files->setCurrentRow(3);if(!body->toPlainText().contains(QStringLiteral("[RUNNER]")))return false;logs.last()->close();
    bool cancelled=false,applied=false;
    for(auto button:findChildren<QPushButton*>())if(button->text()==QStringLiteral("配置阶段")){
        const auto original=selectedFlowConfig();
        QTimer::singleShot(0,this,[this,&cancelled]{auto dialogs=findChildren<QDialog*>("flowDialog");if(!dialogs.isEmpty()){for(auto check:dialogs.last()->findChildren<QCheckBox*>())if(check->text()=="read_edif"){check->setChecked(true);cancelled=true;}dialogs.last()->reject();}});button->click();
        if(!cancelled||selectedFlowConfig()!=original)return false;
        QTimer::singleShot(0,this,[this,&applied]{auto dialogs=findChildren<QDialog*>("flowDialog");if(!dialogs.isEmpty()){for(auto check:dialogs.last()->findChildren<QCheckBox*>())if(check->text()=="read_edif"){check->setChecked(true);applied=true;}dialogs.last()->accept();}});button->click();
        if(!applied||selectedFlowConfig().value("read_edif",0)!=1)return false;resetFlowOptions();break;
    }
    return regressionTable_->rowCount()==5&&cancelled&&applied;
}

bool MainWindow::saveReview(const QString& directory) {
    if(!demo_)return false;QDir output(directory);if(!output.exists()&&!QDir().mkpath(directory))return false;
    resize(1440,900);bool ok=true;
    auto settle=[](){QEventLoop loop;QTimer::singleShot(240,&loop,&QEventLoop::quit);loop.exec();};
    const auto nav=findChildren<QPushButton*>("navigation");const QStringList names{"workers","tasks","add","regression"};
    for(int i=0;i<nav.size();++i){nav[i]->click();settle();ok=grab().save(output.filePath(names[i]+".png"))&&ok;}
    openBatch("demo-batch-1");settle();auto batches=findChildren<QDialog*>("batchDialog");if(!batches.isEmpty()){ok=batches.last()->grab().save(output.filePath("batch-detail.png"))&&ok;
        for(auto button:batches.last()->findChildren<QPushButton*>())if(button->text()==QStringLiteral("导出匹配清单…")){QTimer::singleShot(240,this,[this,&ok,output]{auto dialogs=findChildren<QDialog*>("exportDialog");if(!dialogs.isEmpty()){ok=dialogs.last()->grab().save(output.filePath("export.png"))&&ok;dialogs.last()->reject();}else ok=false;});button->click();break;}
        batches.last()->close();}
    openLogs(tasks_[0]);settle();auto logs=findChildren<QDialog*>("logDialog");if(!logs.isEmpty()){ok=logs.last()->grab().save(output.filePath("original-run.png"))&&ok;logs.last()->close();}
    nav[2]->click();settle();
    for(auto button:findChildren<QPushButton*>())if(button->text()==QStringLiteral("配置阶段")){
        QTimer::singleShot(240,this,[this,&ok,output]{auto dialogs=findChildren<QDialog*>("flowDialog");if(!dialogs.isEmpty()){ok=dialogs.last()->grab().save(output.filePath("flow-config.png"))&&ok;dialogs.last()->reject();}else ok=false;});button->click();break;
    }
    return ok;
}
bool MainWindow::snapshotProbe() const {
    if(demo_||loading_||tasksStale_||tasks_.empty()||tasksTree_->columnCount()!=8)return false;
    size_t displayed=0;for(int i=0;i<tasksTree_->topLevelItemCount();++i)displayed+=tasksTree_->topLevelItem(i)->childCount();
    if(displayed!=tasks_.size())return false;
    for(const auto& task:tasks_){if(!task.contains("created_at_ms")||!task.contains("payload")||!task["payload"].contains("case")||!task["payload"].contains("context")||!task["payload"]["context"].contains("source"))return false;}
    size_t cases=0;for(const auto& task:tasks_)cases+=task.contains("group_summary")?task["group_summary"]["total"].get<size_t>():1;
    std::fprintf(stderr,"QT_SNAPSHOT_PROBE PASS tasks=%zu batches=%d\n",cases,tasksTree_->topLevelItemCount());return true;
}
