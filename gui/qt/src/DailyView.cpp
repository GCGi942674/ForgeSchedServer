#include "ConsoleSupport.h"

void MainWindow::queryDaily(bool next) {
    if(regressionLoading_||loading_||submitting_){note(QStringLiteral("正在执行其他请求，请稍后再查询"));return;}
    if(!operations_.contains("query_regression")){note(QStringLiteral("Server 未声明回归查询能力"),true);return;}
    Json request={{"baseline_date",dateA_->date().toString("yyyy-MM-dd").toStdString()},
        {"date",dateB_->date().toString("yyyy-MM-dd").toStdString()},{"limit",50}};
    const auto stage=regressionStage_->currentData().toString(),change=regressionChange_->currentData().toString();
    if(!stage.isEmpty())request["template"]=stage.toStdString();if(!change.isEmpty())request["change"]=change.toStdString();
    if(!regressionSearch_->text().trimmed().isEmpty())request["search"]=regressionSearch_->text().trimmed().toStdString();
    const auto signature=request.dump();
    if(next&&signature!=regressionFilter_){regressionSummary_->setText(QStringLiteral("筛选已改变，请点击比较 / 首页重新查询。"));return;}
    if(!next)regressionFilter_=signature;
    if(next&&!regressionCursor_.isEmpty())request["after_key"]=regressionCursor_.toStdString();
    regressionLoading_=true;regressionNext_->setEnabled(false);
    dateA_->setEnabled(false);dateB_->setEnabled(false);regressionStage_->setEnabled(false);
    regressionChange_->setEnabled(false);regressionSearch_->setEnabled(false);
    rpc_.request("query_regression",request,[this,next,stage](RpcReply reply){
        regressionLoading_=false;
        dateA_->setEnabled(true);dateB_->setEnabled(true);regressionStage_->setEnabled(true);
        regressionChange_->setEnabled(true);regressionSearch_->setEnabled(true);
        if(!reply.ok){regressionSummary_->setText(QStringLiteral("查询失败，原回归数据已过期：")+reply.error);note(reply.error,true);return;}
        try {
            const auto generation=reply.result.at("snapshot_generation").get<uint64_t>();
            if(next&&(generation!=regressionGeneration_||reply.instance!=regressionInstance_)){
                regressionSummary_->setText(QStringLiteral("数据已变化，请点击比较 / 首页重新查询，避免跨页混用。"));return;}
            regressionGeneration_=generation;regressionInstance_=reply.instance;
            regressionRows_=reply.result.at("items");regressionTable_->setRowCount(0);
            QSignalBlocker block(regressionStage_);regressionStage_->clear();regressionStage_->addItem(QStringLiteral("全部阶段"),"");
            for(const auto& name:reply.result.at("templates")){auto s=QString::fromStdString(name.get<std::string>());regressionStage_->addItem(s,s);}
            const auto index=regressionStage_->findData(stage);if(index>=0)regressionStage_->setCurrentIndex(index);
            for(const auto& item:regressionRows_){int row=regressionTable_->rowCount();regressionTable_->insertRow(row);
                cell(regressionTable_,row,0,text(item,"template"));cell(regressionTable_,row,1,text(item,"case"));
                cell(regressionTable_,row,2,item["baseline"].is_null()?QStringLiteral("缺少结果"):text(item["baseline"],"outcome"));
                cell(regressionTable_,row,3,item["comparison"].is_null()?QStringLiteral("缺少结果"):text(item["comparison"],"outcome"));
                const QMap<QString,QString> changes{{"REGRESSION",QStringLiteral("新回退")},{"IMPROVEMENT",QStringLiteral("新通过")},
                    {"MISSING",QStringLiteral("缺少结果")},{"EXCEPTION",QStringLiteral("异常 / 未完成")},{"UNCHANGED",QStringLiteral("无确认回退")}};
                cell(regressionTable_,row,4,changes.value(text(item,"change")));
                auto controls=new QWidget;auto actions=new QHBoxLayout(controls);actions->setContentsMargins(2,2,2,2);
                for(const auto& side:QList<QPair<QString,QString>>{{QStringLiteral("基准 run"),"baseline"},{QStringLiteral("对比 run"),"comparison"}}){auto button=new QPushButton(side.first);actions->addWidget(button);const auto task=item[side.second.toStdString()];button->setEnabled(!task.is_null());
                    connect(button,&QPushButton::clicked,this,[this,task]{rpc_.request("query_task",{{"task_id",task.at("task_id")}},[this](RpcReply reply){if(reply.ok)openLogs(reply.result);else note(reply.error,true);});});}
                regressionTable_->setCellWidget(row,5,controls);
            }
            regressionCursor_=reply.result["next_after_key"].is_null()?QString{}:QString::fromStdString(reply.result["next_after_key"].get<std::string>());
            regressionNext_->setEnabled(reply.result.at("has_more").get<bool>());
            const auto& counts=reply.result.at("counts");
            QString summary=QStringLiteral("匹配 %1 条 · 本页 %2 条 · 新回退 %3 · 新通过 %4 · 缺少 %5 · 异常 %6")
                .arg(reply.result["total"].get<uint64_t>()).arg(regressionRows_.size())
                .arg(counts["REGRESSION"].get<uint64_t>()).arg(counts["IMPROVEMENT"].get<uint64_t>())
                .arg(counts["MISSING"].get<uint64_t>()).arg(counts["EXCEPTION"].get<uint64_t>());
            for(auto it=reply.result["stages"].begin();it!=reply.result["stages"].end();++it){const auto& stats=it.value();
                summary+=QStringLiteral("\n%1：共 %2 · 比较日 PASS %3 / FAIL %4 / TIMEOUT %5 · 新回退 %6 · 新通过 %7")
                    .arg(QString::fromStdString(it.key())).arg(stats["total"].get<uint64_t>())
                    .arg(stats["comparison"].value("PASS",uint64_t(0))).arg(stats["comparison"].value("FAIL",uint64_t(0)))
                    .arg(stats["comparison"].value("TIMEOUT",uint64_t(0))).arg(stats["changes"]["REGRESSION"].get<uint64_t>())
                    .arg(stats["changes"]["IMPROVEMENT"].get<uint64_t>());}
            summary+=QStringLiteral("\n同日重复记录 %1（取最新 task_id）；阶段汇总不受搜索 / 变化筛选影响。")
                .arg(reply.result["duplicate_records"].get<uint64_t>());regressionSummary_->setText(summary);
        }catch(const std::exception& error){regressionSummary_->setText(QStringLiteral("回归响应无效，请更新 Server：")+QString::fromUtf8(error.what()));}
    });
}
