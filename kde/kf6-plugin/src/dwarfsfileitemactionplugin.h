// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <KAbstractFileItemActionPlugin>

class DwarfsFileItemActionPlugin final : public KAbstractFileItemActionPlugin {
    Q_OBJECT
public:
    explicit DwarfsFileItemActionPlugin(QObject *parent = nullptr);

    QList<QAction*> actions(const KFileItemListProperties& fileItemInfos,
                            QWidget* parentWidget) override;
};
