#include "RecordingPanel.hpp"

#include <OBSApp.hpp>
#include <settings/OBSHotkeyEdit.hpp>
#include <settings/OBSHotkeyWidget.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>

#include <string>

#include "moc_RecordingPanel.cpp"

namespace {
/* Frontend hotkeys shown on the main window, in display order */
constexpr const char *kHotkeyNames[] = {
	"OBSBasic.StartRecording",   "OBSBasic.StopRecording",    "OBSBasic.StartReplayBuffer",
	"OBSBasic.StopReplayBuffer", "OBSBasic.SaveReplayBuffer",
};

struct HotkeyInfo {
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
	QString description;
};

HotkeyInfo FindFrontendHotkey(const char *name)
{
	std::pair<const char *, HotkeyInfo> data{name, {}};

	obs_enum_hotkeys(
		[](void *param, obs_hotkey_id id, obs_hotkey_t *key) {
			auto data = static_cast<std::pair<const char *, HotkeyInfo> *>(param);
			if (obs_hotkey_get_registerer_type(key) == OBS_HOTKEY_REGISTERER_FRONTEND &&
			    strcmp(obs_hotkey_get_name(key), data->first) == 0) {
				data->second.id = id;
				data->second.description = QT_UTF8(obs_hotkey_get_description(key));
				return false;
			}
			return true;
		},
		&data);

	return data.second;
}

std::vector<obs_key_combination_t> GetHotkeyCombinations(obs_hotkey_id id)
{
	std::pair<obs_hotkey_id, std::vector<obs_key_combination_t>> data{id, {}};

	obs_enum_hotkey_bindings(
		[](void *param, size_t, obs_hotkey_binding_t *binding) {
			auto data = static_cast<std::pair<obs_hotkey_id, std::vector<obs_key_combination_t>> *>(param);
			if (obs_hotkey_binding_get_hotkey_id(binding) == data->first) {
				data->second.push_back(obs_hotkey_binding_get_key_combination(binding));
			}
			return true;
		},
		&data);

	return data.second;
}
} // namespace

RecordingPanel::RecordingPanel(OBSBasic *main_) : QFrame(nullptr), main(main_)
{
	pathEdit = new QLineEdit(this);

	QPushButton *browseButton = new QPushButton(QTStr("Browse"), this);
	QPushButton *openButton = new QPushButton(QTStr("Basic.Recording.OpenFolder"), this);

	QHBoxLayout *pathLayout = new QHBoxLayout();
	pathLayout->setContentsMargins(0, 0, 0, 0);
	pathLayout->addWidget(pathEdit, 1);
	pathLayout->addWidget(browseButton);
	pathLayout->addWidget(openButton);

	QWidget *hotkeysWidget = new QWidget(this);
	hotkeyLayout = new QGridLayout(hotkeysWidget);
	hotkeyLayout->setContentsMargins(0, 0, 0, 0);
	hotkeyLayout->setColumnStretch(1, 1);

	QFormLayout *layout = new QFormLayout(this);
	layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	layout->addRow(QTStr("Basic.Recording.Folder"), pathLayout);
	layout->addRow(hotkeysWidget);

	connect(pathEdit, &QLineEdit::editingFinished, this, [this]() { SetOutputPath(pathEdit->text()); });
	connect(browseButton, &QPushButton::clicked, this, [this]() {
		QString dir = SelectDirectory(this, QTStr("Basic.Settings.Output.SelectDirectory"), pathEdit->text());
		if (!dir.isEmpty()) {
			SetOutputPath(dir);
		}
	});
	connect(openButton, &QPushButton::clicked, this,
		[this]() { QDesktopServices::openUrl(QUrl::fromLocalFile(pathEdit->text())); });

	/* Global hotkeys must not fire while a new key combination is being typed in */
	connect(qApp, &QApplication::focusChanged, this,
		[this](QWidget *, QWidget *focused) { UpdateHotkeyFocus(focused); });
}

RecordingPanel::~RecordingPanel()
{
	if (hotkeysDisabled) {
		App()->UpdateHotkeyFocusSetting();
	}
}

void RecordingPanel::RefreshOutputPath()
{
	const char *path = main->GetCurrentOutputPath();
	pathEdit->setText(QT_UTF8(path ? path : ""));
}

void RecordingPanel::SetOutputPath(const QString &path)
{
	QString cleanPath = QDir::toNativeSeparators(QDir::cleanPath(path));
	if (cleanPath.isEmpty()) {
		RefreshOutputPath();
		return;
	}

	main->SetCurrentOutputPath(QT_TO_UTF8(QDir::fromNativeSeparators(cleanPath)));
	RefreshOutputPath();
}

void RecordingPanel::RefreshHotkeys()
{
	for (QPointer<QWidget> &widget : hotkeyWidgets) {
		if (widget) {
			widget->deleteLater();
		}
	}
	hotkeyWidgets.clear();

	int row = 0;
	for (const char *name : kHotkeyNames) {
		HotkeyInfo info = FindFrontendHotkey(name);
		if (info.id == OBS_INVALID_HOTKEY_ID) {
			continue;
		}

		QWidget *container = hotkeyLayout->parentWidget();
		QLabel *label = new QLabel(info.description, container);
		OBSHotkeyWidget *hotkey =
			new OBSHotkeyWidget(container, info.id, name, nullptr, GetHotkeyCombinations(info.id));

		connect(hotkey, &OBSHotkeyWidget::KeyChanged, this, [this, hotkey]() { SaveHotkey(hotkey); });

		hotkeyLayout->addWidget(label, row, 0);
		hotkeyLayout->addWidget(hotkey, row, 1);
		hotkeyWidgets.emplace_back(label);
		hotkeyWidgets.emplace_back(hotkey);
		row++;
	}
}

void RecordingPanel::SaveHotkey(OBSHotkeyWidget *hotkey)
{
	if (!hotkey->Changed()) {
		return;
	}

	hotkey->Save();

	OBSDataArrayAutoRelease array = obs_hotkey_save(hotkey->id);
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_array(data, "bindings", array);
	config_set_string(main->Config(), "Hotkeys", hotkey->name.c_str(), obs_data_get_json(data));
	config_save_safe(main->Config(), "tmp", nullptr);
}

void RecordingPanel::UpdateHotkeyFocus(QWidget *focused)
{
	bool editing = qobject_cast<OBSHotkeyEdit *>(focused) && isAncestorOf(focused);

	if (editing && !hotkeysDisabled) {
		App()->DisableHotkeys();
		hotkeysDisabled = true;
	} else if (!editing && hotkeysDisabled) {
		App()->UpdateHotkeyFocusSetting();
		hotkeysDisabled = false;
	}
}
