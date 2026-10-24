#pragma once

#include <QFrame>
#include <QPointer>

class CaptureTarget;
class OBSBasic;
class QComboBox;
class QLabel;
class QPushButton;

/* Main window panel for choosing what is captured: a display, a window or a game, and whose audio is recorded. */
class CapturePanel : public QFrame {
	Q_OBJECT

public:
	explicit CapturePanel(OBSBasic *main);

private:
	OBSBasic *main;
	CaptureTarget *target;

	QPointer<QComboBox> typeList;
	QPointer<QComboBox> targetList;
	QPointer<QPushButton> refreshButton;
	QPointer<QComboBox> audioList;
	QPointer<QLabel> canvasLabel;
	QPointer<QPushButton> settingsButton;

	void RefreshTargets();
	void RefreshState();
	void UpdateCanvasLabel();
};
