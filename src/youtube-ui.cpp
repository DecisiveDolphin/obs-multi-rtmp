#include "youtube-ui.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace {

std::string ToU8(const QString& s)
{
    auto b = s.toUtf8();
    return std::string(b.constData(), b.size());
}

// Runs fn on the UI thread later; fn is skipped if `guard` was destroyed.
template<class W, class F>
void PostToUi(QPointer<W> guard, F&& fn)
{
    QMetaObject::invokeMethod(qApp, [guard, fn = std::forward<F>(fn)]() mutable {
        if (guard)
            fn(guard.data());
    }, Qt::QueuedConnection);
}

const char* kSetupHelp =
    "To let this plugin talk to YouTube you need your own free Google API client:<br>"
    "1. Open <a href=\"https://console.cloud.google.com/\">console.cloud.google.com</a> and create a project.<br>"
    "2. Enable <b>YouTube Data API v3</b> (APIs &amp; Services &gt; Library).<br>"
    "3. Set up the <b>OAuth consent screen</b> (External), add yourself as a test user, "
    "then click <b>Publish app</b> so sign-ins don't expire after 7 days.<br>"
    "4. Credentials &gt; Create credentials &gt; OAuth client ID &gt; type "
    "<b>TVs and Limited Input devices</b>.<br>"
    "5. Paste the Client ID and Client secret below.";

} // namespace

YouTubeTab::YouTubeTab(QWidget* parent)
    : QWidget(parent)
{
    auto root = new QVBoxLayout(this);

    if (!youtube::IsSupported()) {
        root->addWidget(new QLabel("YouTube integration is only available in the Windows build.", this));
    }

    // Account row
    {
        auto row = new QHBoxLayout();
        account_ = new QLabel(this);
        account_->setWordWrap(true);
        row->addWidget(account_, 1);
        signIn_ = new QPushButton("Sign in", this);
        signOut_ = new QPushButton("Sign out", this);
        auto setup = new QPushButton("API setup...", this);
        row->addWidget(signIn_);
        row->addWidget(signOut_);
        row->addWidget(setup);
        root->addLayout(row);

        connect(signIn_, &QPushButton::clicked, this, [this]() { StartSignIn(); });
        connect(signOut_, &QPushButton::clicked, this, [this]() {
            auto auth = youtube::LoadAuth();
            auth.refreshToken.clear();
            auth.channelTitle.clear();
            youtube::SaveAuth(auth);
            UpdateAccountRow();
        });
        connect(setup, &QPushButton::clicked, this, [this]() { ShowApiSetup(); });
    }

    enabled_ = new QCheckBox("Create a YouTube broadcast with these details when this output starts", this);
    root->addWidget(enabled_);

    fields_ = new QWidget(this);
    auto form = new QFormLayout(fields_);
    form->setContentsMargins(0, 0, 0, 0);

    title_ = new QLineEdit(fields_);
    title_->setMaxLength(100);
    title_->setPlaceholderText("Stream title (max 100 characters)");
    form->addRow("Title", title_);

    description_ = new QPlainTextEdit(fields_);
    description_->setPlaceholderText("Description");
    description_->setMinimumHeight(80);
    description_->setMaximumHeight(140);
    form->addRow("Description", description_);

    privacy_ = new QComboBox(fields_);
    privacy_->addItem("Public", "public");
    privacy_->addItem("Unlisted", "unlisted");
    privacy_->addItem("Private", "private");
    form->addRow("Visibility", privacy_);

    madeForKids_ = new QCheckBox("Made for kids", fields_);
    form->addRow("", madeForKids_);

    {
        auto row = new QHBoxLayout();
        thumbnail_ = new QLineEdit(fields_);
        thumbnail_->setPlaceholderText("JPG or PNG, under 2 MB, 1280x720 recommended");
        browse_ = new QPushButton("Browse...", fields_);
        auto clear = new QPushButton("Clear", fields_);
        row->addWidget(thumbnail_, 1);
        row->addWidget(browse_);
        row->addWidget(clear);
        form->addRow("Thumbnail", row);

        connect(browse_, &QPushButton::clicked, this, [this]() {
            auto start = thumbnail_->text().isEmpty() ? QString() : QFileInfo(thumbnail_->text()).absolutePath();
            auto file = QFileDialog::getOpenFileName(this, "Choose thumbnail", start, "Images (*.jpg *.jpeg *.png)");
            if (!file.isEmpty())
                thumbnail_->setText(file);
        });
        connect(clear, &QPushButton::clicked, thumbnail_, &QLineEdit::clear);
        connect(thumbnail_, &QLineEdit::textChanged, this, [this]() { UpdateThumbPreview(); });
    }

    thumbPreview_ = new QLabel(fields_);
    thumbPreview_->setFixedSize(192, 108);
    thumbPreview_->setAlignment(Qt::AlignCenter);
    thumbPreview_->setStyleSheet("QLabel { border: 1px solid rgba(128,128,128,0.5); }");
    form->addRow("", thumbPreview_);

    root->addWidget(fields_);

    auto note = new QLabel(
        "A new broadcast is created and attached to this target's stream key each time it starts, "
        "and it ends automatically when you stop. Custom thumbnails need a verified YouTube channel.",
        this);
    note->setWordWrap(true);
    note->setStyleSheet("QLabel { color: gray; }");
    root->addWidget(note);
    root->addStretch(1);

    connect(enabled_, &QCheckBox::toggled, this, [this]() { UpdateEnabledState(); });

    UpdateAccountRow();
    UpdateEnabledState();
    UpdateThumbPreview();
}

void YouTubeTab::Load(const YouTubeTargetConfig& cfg)
{
    enabled_->setChecked(cfg.enabled);
    title_->setText(QString::fromStdString(cfg.title));
    description_->setPlainText(QString::fromStdString(cfg.description));
    int idx = privacy_->findData(QString::fromStdString(cfg.privacy));
    privacy_->setCurrentIndex(idx >= 0 ? idx : 0);
    madeForKids_->setChecked(cfg.madeForKids);
    thumbnail_->setText(QString::fromStdString(cfg.thumbnailPath));
    UpdateEnabledState();
}

void YouTubeTab::Save(YouTubeTargetConfig& cfg) const
{
    cfg.enabled = enabled_->isChecked();
    cfg.title = ToU8(title_->text().trimmed());
    cfg.description = ToU8(description_->toPlainText());
    cfg.privacy = ToU8(privacy_->currentData().toString());
    cfg.madeForKids = madeForKids_->isChecked();
    cfg.thumbnailPath = ToU8(thumbnail_->text().trimmed());
}

void YouTubeTab::UpdateEnabledState()
{
    fields_->setEnabled(enabled_->isChecked());
}

void YouTubeTab::UpdateThumbPreview()
{
    QPixmap pm(thumbnail_->text().trimmed());
    if (pm.isNull()) {
        thumbPreview_->setPixmap(QPixmap());
        thumbPreview_->setText(thumbnail_->text().trimmed().isEmpty() ? "No thumbnail" : "Can't load image");
    } else {
        thumbPreview_->setText("");
        thumbPreview_->setPixmap(pm.scaled(thumbPreview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

void YouTubeTab::UpdateAccountRow()
{
    auto auth = youtube::LoadAuth();
    bool supported = youtube::IsSupported();
    if (auth.clientId.empty() || auth.clientSecret.empty()) {
        account_->setText("YouTube API not set up yet. Click <b>API setup...</b> first.");
    } else if (auth.refreshToken.empty()) {
        account_->setText("Not signed in.");
    } else {
        auto name = auth.channelTitle.empty() ? QString("your channel") : QString::fromStdString(auth.channelTitle).toHtmlEscaped();
        account_->setText("Signed in as <b>" + name + "</b>");
    }
    signIn_->setEnabled(supported);
    signIn_->setVisible(auth.refreshToken.empty());
    signOut_->setVisible(!auth.refreshToken.empty());
}

void YouTubeTab::ShowApiSetup()
{
    auto auth = youtube::LoadAuth();

    QDialog dlg(this);
    dlg.setWindowTitle("YouTube API setup");
    auto layout = new QVBoxLayout(&dlg);
    auto help = new QLabel(kSetupHelp, &dlg);
    help->setWordWrap(true);
    help->setOpenExternalLinks(true);
    help->setTextFormat(Qt::RichText);
    layout->addWidget(help);

    auto form = new QFormLayout();
    auto id = new QLineEdit(QString::fromStdString(auth.clientId), &dlg);
    auto secret = new QLineEdit(QString::fromStdString(auth.clientSecret), &dlg);
    secret->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    form->addRow("Client ID", id);
    form->addRow("Client secret", secret);
    layout->addLayout(form);

    auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    dlg.resize(560, dlg.sizeHint().height());

    if (dlg.exec() != QDialog::Accepted)
        return;

    auto newId = ToU8(id->text().trimmed());
    auto newSecret = ToU8(secret->text().trimmed());
    if (newId != auth.clientId || newSecret != auth.clientSecret) {
        // A token from a different client won't refresh, so drop it.
        auth.refreshToken.clear();
        auth.channelTitle.clear();
    }
    auth.clientId = newId;
    auth.clientSecret = newSecret;
    youtube::SaveAuth(auth);
    UpdateAccountRow();
}

void YouTubeTab::StartSignIn()
{
    auto auth = youtube::LoadAuth();
    if (auth.clientId.empty() || auth.clientSecret.empty()) {
        ShowApiSetup();
        auth = youtube::LoadAuth();
        if (auth.clientId.empty() || auth.clientSecret.empty())
            return;
    }

    auto dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("Sign in to YouTube");
    auto layout = new QVBoxLayout(dlg);
    auto status = new QLabel("Contacting Google...", dlg);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto code = new QLabel(dlg);
    code->setTextInteractionFlags(Qt::TextSelectableByMouse);
    code->setAlignment(Qt::AlignCenter);
    auto f = code->font();
    f.setPointSize(f.pointSize() * 2);
    f.setBold(true);
    code->setFont(f);
    layout->addWidget(code);
    auto open = new QPushButton("Open sign-in page", dlg);
    open->setEnabled(false);
    layout->addWidget(open);
    auto cancelBtn = new QPushButton("Cancel", dlg);
    layout->addWidget(cancelBtn);
    connect(cancelBtn, &QPushButton::clicked, dlg, &QDialog::reject);
    dlg->resize(420, dlg->sizeHint().height());

    auto cancelled = std::make_shared<std::atomic_bool>(false);
    connect(dlg, &QDialog::finished, dlg, [cancelled](int) { *cancelled = true; });

    QPointer<QDialog> guardDlg(dlg);
    QPointer<YouTubeTab> guardTab(this);

    std::thread([auth, cancelled, guardDlg, guardTab, status, code, open]() {
        std::string error;
        auto dc = youtube::RequestDeviceCode(auth.clientId, error);
        if (!dc) {
            PostToUi(guardDlg, [status, error](QDialog*) {
                status->setText("Couldn't start sign-in: " + QString::fromStdString(error));
            });
            return;
        }

        auto userCode = QString::fromStdString(dc->userCode);
        auto url = QString::fromStdString(dc->verificationUrl);
        PostToUi(guardDlg, [status, code, open, userCode, url](QDialog*) {
            code->setText(userCode);
            status->setText("Go to <b>" + url.toHtmlEscaped() + "</b> and enter this code "
                "(it's already copied to your clipboard). Then pick your YouTube channel and allow access.");
            QApplication::clipboard()->setText(userCode);
            open->setEnabled(true);
            QObject::connect(open, &QPushButton::clicked, open, [url]() { QDesktopServices::openUrl(QUrl(url)); });
            QDesktopServices::openUrl(QUrl(url));
        });

        int interval = dc->interval > 0 ? dc->interval : 5;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(dc->expiresIn);
        while (!*cancelled && std::chrono::steady_clock::now() < deadline) {
            for (int i = 0; i < interval * 4 && !*cancelled; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (*cancelled)
                return;

            std::string refresh, channel;
            auto r = youtube::PollDeviceToken(auth.clientId, auth.clientSecret, dc->deviceCode, refresh, channel, error);
            if (r == youtube::PollResult::Pending)
                continue;
            if (r == youtube::PollResult::SlowDown) {
                interval += 5;
                continue;
            }
            if (r == youtube::PollResult::Granted) {
                auto saved = youtube::LoadAuth();
                saved.refreshToken = refresh;
                saved.channelTitle = channel;
                youtube::SaveAuth(saved);
                PostToUi(guardTab, [](YouTubeTab* tab) { tab->UpdateAccountRow(); });
                PostToUi(guardDlg, [](QDialog* d) { d->accept(); });
                return;
            }
            PostToUi(guardDlg, [status, code, error](QDialog*) {
                code->clear();
                status->setText("Sign-in failed: " + QString::fromStdString(error));
            });
            return;
        }
        if (!*cancelled) {
            PostToUi(guardDlg, [status, code](QDialog*) {
                code->clear();
                status->setText("The sign-in code expired. Close this and try again.");
            });
        }
    }).detach();

    dlg->open();
}
