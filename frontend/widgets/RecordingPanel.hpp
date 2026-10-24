#pragma once

#include <QFrame>
#include <QPointer>

#include <vector>

class OBSBasic;
class OBSHotkeyWidget;
class QGridLayout;
class QLineEdit;

/* Main window panel with the recording folder and the hotkeys for recording and the replay buffer. */
class RecordingPanel : public QFrame {
	Q_OBJECT

public:
	explicit RecordingPanel(OBSBasic *main);
	~RecordingPanel();

	/* Called whenever the output settings or the frontend hotkeys may have been recreated */
	void RefreshOutputPath();
	void RefreshHotkeys();

private:
	OBSBasic *main;

	QPointer<QLineEdit> pathEdit;
	QPointer<QGridLayout> hotkeyLayout;
	std::vector<QPointer<QWidget>> hotkeyWidgets;
	bool hotkeysDisabled = false;

	void SetOutputPath(const QString &path);
	void SaveHotkey(OBSHotkeyWidget *widget);
	void UpdateHotkeyFocus(QWidget *focused);
};
