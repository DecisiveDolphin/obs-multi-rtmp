#pragma once

#include <QWidget>
#include "youtube-api.h"

class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QComboBox;
class QLabel;
class QPushButton;

// "YouTube" tab in the target's edit dialog.
class YouTubeTab : public QWidget {
public:
    explicit YouTubeTab(QWidget* parent);

    void Load(const YouTubeTargetConfig& cfg);
    void Save(YouTubeTargetConfig& cfg) const;

private:
    void UpdateAccountRow();
    void UpdateEnabledState();
    void ShowApiSetup();
    void StartSignIn();

    QCheckBox* enabled_ = nullptr;
    QLineEdit* title_ = nullptr;
    QPlainTextEdit* description_ = nullptr;
    QComboBox* privacy_ = nullptr;
    QCheckBox* madeForKids_ = nullptr;
    QLineEdit* thumbnail_ = nullptr;
    QPushButton* browse_ = nullptr;
    QLabel* thumbPreview_ = nullptr;

    QLabel* account_ = nullptr;
    QPushButton* signIn_ = nullptr;
    QPushButton* signOut_ = nullptr;
    QWidget* fields_ = nullptr;

    void UpdateThumbPreview();
};
