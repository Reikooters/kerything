// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_SEARCHRESULTTABLEVIEW_H
#define KERYTHING_SEARCHRESULTTABLEVIEW_H

#include <QModelIndex>
#include <QTableView>

class SearchResultTableView final : public QTableView {
    Q_OBJECT

public:
    explicit SearchResultTableView(QWidget* parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    QModelIndex dragPressIndex_;
    bool dragPressCanDrag_ = false;
};

#endif // KERYTHING_SEARCHRESULTTABLEVIEW_H