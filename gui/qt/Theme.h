#pragma once
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QPainter>
#include <QIcon>
#include <QVBoxLayout>
#include <QStackedWidget>
#include <QLabel>

namespace GcrpUi {
inline QString styleSheet() {
    return QStringLiteral(R"QSS(
QWidget { font-family: "Segoe UI", "Noto Sans CJK SC", "Microsoft YaHei", sans-serif; font-size: 13px; color: #26354b; }
QMainWindow, QWidget#surface, QWidget#workspace { background: #f5f7fb; }
QWidget#sidebar { background: #edf2f9; border-right: 1px solid #dde5f0; }
QLabel#brand { font-size: 21px; font-weight: 600; color: #172c48; }
QLabel#brandBadge { background: #2d66cc; color: white; border-radius: 10px; font-size: 20px; font-weight: 600; }
QLabel#pageTitle { font-size: 24px; font-weight: 600; color: #182e4b; padding-top: 2px; }
QLabel#muted { color: #6b7c93; }
QLabel#sidebarFooter { color: #73839a; font-size: 12px; padding: 14px 2px; border-top: 1px solid #dbe4ef; }
QPushButton {
    background: #ffffff; border: 1px solid #d9e2ef; border-radius: 9px;
    padding: 9px 15px; min-height: 18px; color: #41546d;
}
QPushButton:hover { background: #f0f5fc; border-color: #b7ccec; color: #245cba; }
QPushButton:pressed { background: #dfeafb; border-color: #91b3e8; color: #184b9d; padding-top: 10px; padding-bottom: 8px; }
QPushButton:focus { border-color: #739ee3; }
QPushButton[kind="primary"] { background: #2d66cc; border-color: #2d66cc; color: white; font-weight: 600; }
QPushButton[kind="primary"]:hover { background: #255abd; border-color: #255abd; }
QPushButton[kind="primary"]:pressed { background: #1d4c9f; border-color: #1d4c9f; }
QPushButton[kind="primary"]:focus { border: 1px solid #a4c3f8; }
QPushButton[kind="danger"] { background: #fff7f8; border-color: #efd5da; color: #ab4052; }
QPushButton[kind="danger"]:hover { background: #fdecef; border-color: #dfa8b4; }
QPushButton[kind="danger"]:pressed { background: #f8dce3; }
QPushButton#navigation { text-align: left; background: transparent; border: 1px solid transparent; border-radius: 10px; padding: 12px 13px; margin-top: 5px; color: #657994; }
QPushButton#navigation:hover { background: #e2eaf6; color: #284e86; }
QPushButton#navigation:pressed { background: #d4e2f7; }
QPushButton#navigation:checked { background: #dfeaff; border-color: #ccdcf6; color: #245ec0; font-weight: 600; }
QPushButton#navigation:checked:hover { background: #d4e3fc; }
QPushButton:disabled, QPushButton[kind="primary"]:disabled, QPushButton[kind="danger"]:disabled {
    background: #edf1f6; color: #94a1b3; border-color: #e1e7f0;
}
QFrame#contentCard { background: white; border: 1px solid #e1e8f1; border-radius: 13px; }
QTreeWidget, QTableWidget, QPlainTextEdit {
    background: white; border: 0; alternate-background-color: #f8fafe;
    selection-background-color: #e4eeff; selection-color: #224f93;
    outline: 0; padding: 3px;
}
QTreeWidget::item, QTableWidget::item { padding: 10px 8px; border-bottom: 1px solid #edf1f7; }
QTreeWidget::item:hover, QTableWidget::item:hover { background: #eef4fd; }
QTreeWidget::item:selected, QTableWidget::item:selected { background: #e1edff; color: #2459a7; }
QTreeWidget::item:focus, QTableWidget::item:focus { border: 1px solid #b6cff3; }
QHeaderView::section { background: #f1f5fb; color: #6c7e96; border: 0; border-bottom: 1px solid #e3eaf4; padding: 12px 10px; font-weight: 500; }
QTableCornerButton::section { background: #f1f5fb; border: 0; }
QLineEdit, QSpinBox, QComboBox, QDateEdit {
    background: white; border: 1px solid #d8e2ef; border-radius: 9px; padding: 8px 10px; min-height: 18px;
    selection-background-color: #d9e8ff; selection-color: #194782;
}
QLineEdit:hover, QSpinBox:hover, QComboBox:hover, QDateEdit:hover { border-color: #b8cce8; }
QLineEdit:focus, QSpinBox:focus, QComboBox:focus, QDateEdit:focus { border: 1px solid #6998e8; background: #fbfdff; }
QComboBox::drop-down, QDateEdit::drop-down { border: 0; width: 26px; }
QComboBox QAbstractItemView { background: white; border: 1px solid #dbe5f2; padding: 6px; selection-background-color: #e5efff; selection-color: #2459a7; }
QGroupBox { background: white; border: 1px solid #dfe7f2; border-radius: 13px; margin-top: 14px; padding: 20px 12px 12px; }
QGroupBox::title { subcontrol-origin: margin; left: 16px; padding: 0 7px; color: #3e587b; font-weight: 600; }
QSplitter::handle { background: transparent; width: 14px; height: 14px; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: #c7d3e3; border-radius: 4px; min-height: 35px; }
QScrollBar::handle:vertical:hover { background: #9eb6d5; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: #c7d3e3; border-radius: 4px; min-width: 35px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }
QToolTip { background: #233b5a; color: white; border: 0; padding: 8px; }
QDialog, QMessageBox { background: #f5f7fb; }
)QSS");
}
inline QWidget* card(QWidget* content) {
    auto panel = new QFrame; panel->setObjectName("contentCard");
    auto layout = new QVBoxLayout(panel); layout->setContentsMargins(10,10,10,10);
    layout->addWidget(content);
    auto shadow = new QGraphicsDropShadowEffect(panel);
    shadow->setBlurRadius(18); shadow->setOffset(0,3); shadow->setColor(QColor(36,61,101,18));
    panel->setGraphicsEffect(shadow); return panel;
}
// Fade a captured old page above the new page. Applying opacity to the entire
// stack nests effects around the cards' shadows and breaks Qt5 raster painting.
class PageTransition : public QObject {
public:
    PageTransition(QStackedWidget* pages, QObject* owner) : QObject(owner), pages_(pages) {
        overlay_ = new QLabel(pages); overlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
        overlay_->hide();
        opacity_ = new QGraphicsOpacityEffect(overlay_); overlay_->setGraphicsEffect(opacity_);
        animation_ = new QPropertyAnimation(opacity_,"opacity",this);
        animation_->setDuration(140); animation_->setStartValue(1.0); animation_->setEndValue(0.0);
        animation_->setEasingCurve(QEasingCurve::OutCubic);
        connect(animation_,&QPropertyAnimation::finished,this,[this]{overlay_->hide();});
    }
    void capture() {
        animation_->stop(); overlay_->hide();
        overlay_->setPixmap(pages_->grab()); overlay_->setGeometry(pages_->rect());
    }
    void start() { overlay_->show(); overlay_->raise(); opacity_->setOpacity(1.0); animation_->start(); }
private:
    QStackedWidget* pages_;
    QLabel* overlay_;
    QGraphicsOpacityEffect* opacity_;
    QPropertyAnimation* animation_;
};
inline PageTransition* pageTransition(QStackedWidget* pages, QObject* owner) {
    return new PageTransition(pages,owner);
}
inline QIcon navigationIcon(int page) {
    QPixmap pixmap(40,40); pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#527caf"),2.2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    if(page==0) {
        painter.drawRoundedRect(QRectF(6,7,28,11),3,3); painter.drawRoundedRect(QRectF(6,23,28,11),3,3);
        painter.drawPoint(QPointF(11,12)); painter.drawPoint(QPointF(11,28));
        painter.drawLine(QPointF(24,12),QPointF(29,12)); painter.drawLine(QPointF(24,28),QPointF(29,28));
    } else if(page==1) {
        for(int y:{10,20,30}){painter.drawRoundedRect(QRectF(6,y-3,6,6),1,1);painter.drawLine(QPointF(18,y),QPointF(33,y));}
    } else if(page==2) {
        painter.drawRoundedRect(QRectF(5,11,30,23),3,3); painter.drawLine(QPointF(7,11),QPointF(7,6));
        painter.drawLine(QPointF(7,6),QPointF(17,6)); painter.drawLine(QPointF(17,6),QPointF(22,11));
        painter.drawLine(QPointF(20,17),QPointF(20,28)); painter.drawLine(QPointF(15,22),QPointF(25,22));
    } else {
        painter.drawLine(QPointF(11,7),QPointF(11,33)); painter.drawLine(QPointF(29,7),QPointF(29,33));
        painter.drawLine(QPointF(11,14),QPointF(29,25)); painter.drawEllipse(QPointF(11,7),3,3);
        painter.drawEllipse(QPointF(11,33),3,3); painter.drawEllipse(QPointF(29,7),3,3); painter.drawEllipse(QPointF(29,33),3,3);
    }
    painter.end();return QIcon(pixmap);
}
}
