#pragma once
#include <QApplication>
#include <QFont>
#include <QPalette>

namespace cst {
inline void applyTheme(QApplication &app) {
    app.setStyle("Fusion");
    auto font = app.font(); font.setPointSize(10); app.setFont(font);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#F3F5F9"));
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
        QWidget#contentCard { background: white; border-radius: 10px; }
        QScrollArea { border: none; background: transparent; }
        QLineEdit, QSpinBox, QComboBox { background: white; color: #172B4D; border: 1px solid #CBD5E1; border-radius: 6px; padding: 7px 10px; min-height: 20px; selection-background-color: #2563EB; }
        QLineEdit:hover, QSpinBox:hover, QComboBox:hover { border-color: #94A3B8; }
        QLineEdit:focus, QSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus { border: 1px solid #2563EB; }
        QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled, QPlainTextEdit:disabled { background: #F1F5F9; color: #64748B; border-color: #E2E8F0; }
        QComboBox { padding-right: 28px; }
        QComboBox::drop-down { width: 24px; border: none; }
        QComboBox QAbstractItemView { background: white; color: #172B4D; selection-background-color: #DBEAFE; selection-color: #1D4ED8; padding: 4px; }
        QPlainTextEdit, QListWidget, QTableWidget { background: white; color: #172B4D; border: 1px solid #E2E8F0; border-radius: 8px; selection-background-color: #DBEAFE; selection-color: #1D4ED8; }
        QPlainTextEdit { padding: 10px; }
        QPlainTextEdit[role="code"] { font-family: Consolas, monospace; background: #F8FAFC; }
        QListWidget::item { padding: 8px; border-radius: 5px; }
        QListWidget::item:hover { background: #F1F5F9; }
        QListWidget::item:selected { background: #DBEAFE; color: #1D4ED8; }
        QListWidget#adminSidebar { background: transparent; border: none; }
        QListWidget#adminSidebar::item:selected { font-weight: 600; }
        QTableWidget { gridline-color: #E2E8F0; }
        QHeaderView::section { background: #F8FAFC; color: #475569; border: none; border-bottom: 1px solid #E2E8F0; padding: 8px; }
        QPushButton, QToolButton { background: white; color: #334155; border: 1px solid #CBD5E1; border-radius: 6px; padding: 7px 14px; min-height: 20px; }
        QPushButton:hover, QToolButton:hover { background: #EFF6FF; border-color: #93C5FD; color: #1D4ED8; }
        QPushButton:pressed, QToolButton:pressed { background: #DBEAFE; }
        QPushButton:focus, QToolButton:focus { border: 1px solid #2563EB; }
        QPushButton:checked { background: #DBEAFE; color: #1D4ED8; border-color: #BFDBFE; font-weight: 600; }
        QPushButton[role="primary"] { background: #2563EB; border-color: #2563EB; color: white; }
        QPushButton[role="primary"]:hover { background: #1D4ED8; }
        QPushButton:disabled, QToolButton:disabled { background: #F1F5F9; border-color: #E2E8F0; color: #94A3B8; }
        QCheckBox { spacing: 8px; min-height: 30px; }
        QSplitter::handle { background: #E2E8F0; width: 1px; }
        QStatusBar { background: white; color: #64748B; border-top: 1px solid #E2E8F0; }
        QStatusBar::item { border: none; }
    )"));
}
}
