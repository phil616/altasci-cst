#pragma once
#include <QApplication>
#include <QFont>
#include <QPalette>

inline void initializeUiResources() { Q_INIT_RESOURCE(ui_icons); }

namespace cst {
inline void applyTheme(QApplication &app) {
    initializeUiResources();
    app.setStyle("Fusion");
    auto font = app.font();
    font.setFamilies({"Microsoft YaHei UI", "Microsoft YaHei", "Segoe UI", "Noto Sans CJK SC", "sans-serif"});
    font.setPointSize(10); app.setFont(font);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#EEF2F6"));
    palette.setColor(QPalette::WindowText, QColor("#172B4D"));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::AlternateBase, QColor("#F8FAFC"));
    palette.setColor(QPalette::Text, QColor("#172B4D"));
    palette.setColor(QPalette::Button, Qt::white);
    palette.setColor(QPalette::ButtonText, QColor("#172B4D"));
    palette.setColor(QPalette::Highlight, QColor("#2563EB"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#64748B"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#64748B"));
    app.setPalette(palette);
    app.setStyleSheet(QStringLiteral(R"(
        QToolTip { background: #172B4D; color: white; border: none; padding: 6px; }
        QLabel { background: transparent; }
        QLabel[role="heading"] { font-size: 20px; font-weight: 600; color: #0F172A; }
        QLabel[role="muted"] { color: #64748B; }
        QLabel[role="notice"] { background: #FFFBEB; color: #92400E; border: 1px solid #FDE68A; border-radius: 8px; padding: 12px; }
        QWidget#contentCard, QFrame[role="card"] { background: white; border: 1px solid #CBD5E1; border-radius: 8px; }
        QFrame#appHeader { background: #172B4D; border: none; }
        QFrame#appHeader QLabel { color: white; }
        QFrame#appHeader QPushButton { background: transparent; color: #CBD5E1; border: 1px solid transparent; border-radius: 6px; padding: 8px 16px; min-height: 0; }
        QFrame#appHeader QPushButton:hover { background: rgba(255,255,255,0.12); color: white; border-color: transparent; }
        QFrame#appHeader QPushButton:checked { background: #2563EB; color: white; border-color: #2563EB; font-weight: 600; }
        QFrame#appHeader QPushButton:focus { border-color: #93C5FD; }
        QLabel#projectState { color: white; border-radius: 10px; padding: 4px 10px; background: #64748B; }
        QLabel#operationStatus { color: #475569; padding: 0 8px; }
        QLabel#credentialStatus, QLabel#validationIssues { color: #475569; }
        QLabel[role="previewButton"] { background: white; color: #334155; border: 1px solid #94A3B8; border-radius: 6px; padding: 8px 12px; min-height: 24px; }
        QLabel[role="section"] { color: #172B4D; font-weight: 600; font-size: 15px; padding-top: 8px; }
        QLabel[role="help"] { color: #475569; }
        QLabel[role="status"] { background: #E2E8F0; color: #334155; border-radius: 6px; padding: 8px 12px; }
        QTabWidget::pane { background: white; border: 1px solid #CBD5E1; top: -1px; }
        QTabBar::tab { color: #475569; background: #E2E8F0; border: 1px solid #CBD5E1; padding: 10px 16px; }
        QTabBar::tab:selected { color: #1D4ED8; background: white; border-bottom: 2px solid #2563EB; }
        QTabBar::tab:focus { border-top: 2px solid #2563EB; }
        QGroupBox { background: #F8FAFC; border: 1px solid #CBD5E1; border-radius: 6px; margin-top: 14px; padding: 16px; }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; color: #172B4D; font-weight: 600; padding: 0 4px; }
        QScrollArea { border: none; background: transparent; }
        QLineEdit, QSpinBox, QComboBox { background: white; color: #172B4D; border: 1px solid #94A3B8; border-radius: 6px; padding: 7px 10px; min-height: 20px; selection-background-color: #2563EB; }
        QLineEdit:hover, QSpinBox:hover, QComboBox:hover { border-color: #94A3B8; }
        QLineEdit:focus, QSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus { border: 1px solid #2563EB; }
        QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled, QPlainTextEdit:disabled { background: #F1F5F9; color: #64748B; border-color: #CBD5E1; }
        QComboBox { padding-right: 28px; }
        QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; width: 32px; background: #E2E8F0; border-left: 1px solid #94A3B8; border-top-right-radius: 5px; border-bottom-right-radius: 5px; }
        QComboBox::down-arrow { image: url(:/ui/down.png); width: 16px; height: 16px; }
        QSpinBox { padding-right: 32px; }
        QSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 28px; background: #E2E8F0; border: 1px solid #94A3B8; }
        QSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 28px; background: #E2E8F0; border: 1px solid #94A3B8; }
        QSpinBox::up-arrow { image: url(:/ui/up.png); width: 12px; height: 12px; }
        QSpinBox::down-arrow { image: url(:/ui/down.png); width: 12px; height: 12px; }
        QComboBox QAbstractItemView { background: white; color: #172B4D; selection-background-color: #DBEAFE; selection-color: #1D4ED8; padding: 4px; }
        QPlainTextEdit, QListWidget, QTableWidget { background: white; color: #172B4D; border: 1px solid #CBD5E1; border-radius: 8px; selection-background-color: #DBEAFE; selection-color: #1D4ED8; }
        QPlainTextEdit { padding: 10px; }
        QPlainTextEdit[role="code"] { font-family: Consolas, monospace; background: #F8FAFC; }
        QListWidget::item { padding: 8px; border-radius: 5px; }
        QListWidget::item:hover { background: #F1F5F9; }
        QListWidget::item:selected { background: #DBEAFE; color: #1D4ED8; }
        QListWidget#adminSidebar { background: #172B4D; color: #E2E8F0; border: none; padding: 8px; }
        QListWidget#adminSidebar::item { margin: 2px 0; padding: 10px; }
        QListWidget#adminSidebar::item:hover { background: #284267; color: white; }
        QListWidget#adminSidebar::item:selected { background: #2563EB; color: white; font-weight: 600; }
        QTableWidget { gridline-color: #CBD5E1; }
        QHeaderView::section { background: #F8FAFC; color: #475569; border: none; border-bottom: 1px solid #CBD5E1; padding: 8px; }
        QPushButton, QToolButton { background: white; color: #334155; border: 1px solid #94A3B8; border-radius: 6px; padding: 7px 14px; min-height: 20px; }
        QPushButton:hover, QToolButton:hover { background: #EFF6FF; border-color: #93C5FD; color: #1D4ED8; }
        QPushButton:pressed, QToolButton:pressed { background: #DBEAFE; }
        QPushButton:focus, QToolButton:focus { border: 1px solid #2563EB; }
        QPushButton:checked { background: #DBEAFE; color: #1D4ED8; border-color: #BFDBFE; font-weight: 600; }
        QPushButton[role="primary"] { background: #2563EB; border-color: #2563EB; color: white; }
        QPushButton[role="primary"]:hover { background: #1D4ED8; }
        QPushButton:disabled, QToolButton:disabled { background: #F1F5F9; border-color: #CBD5E1; color: #94A3B8; }
        QCheckBox { spacing: 8px; min-height: 30px; }
        QCheckBox::indicator { width: 18px; height: 18px; background: white; border: 1px solid #64748B; border-radius: 3px; }
        QCheckBox::indicator:checked { background: #2563EB; border-color: #2563EB; image: url(:/ui/check.png); }
        QCheckBox::indicator:disabled { border-color: #94A3B8; background: #CBD5E1; }
        QPushButton[role="danger"] { color: #B91C1C; border-color: #DC2626; background: #FFF7F7; }
        QPushButton[role="danger"]:hover { background: #FEE2E2; }
        QPushButton[role="primary"]:disabled, QPushButton[role="danger"]:disabled { background: #E2E8F0; color: #64748B; border-color: #94A3B8; }
        QSplitter::handle { background: #CBD5E1; width: 1px; }
        QStatusBar { background: white; color: #64748B; border-top: 1px solid #CBD5E1; }
        QStatusBar::item { border: none; }
    )"));
}
}
