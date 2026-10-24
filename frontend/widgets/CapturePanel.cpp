#include "CapturePanel.hpp"

#include <utility/CaptureTarget.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

#include <functional>

#include "moc_CapturePanel.cpp"

namespace {
/* Refreshes its items right before the popup opens, so newly opened windows show up without a manual refresh */
class RefreshingComboBox : public QComboBox {
public:
	using QComboBox::QComboBox;

	std::function<void()> beforePopup;

	void showPopup() override
	{
		if (beforePopup) {
			beforePopup();
		}
		QComboBox::showPopup();
	}
};

/* Window targets are stored as "title:class:exe" with ':' inside each part encoded as "#3A" */
QString DescribeWindowValue(const QString &value)
{
	QStringList parts = value.split(':');
	for (QString &part : parts) {
		part.replace("#3A", ":");
	}

	if (parts.size() >= 3 && !parts[2].isEmpty()) {
		return QString("[%1]: %2").arg(parts[2], parts[0]);
	}
	return parts.value(0);
}
} // namespace

CapturePanel::CapturePanel(OBSBasic *main_) : QFrame(nullptr), main(main_), target(main_->GetCaptureTarget())
{
	typeList = new QComboBox(this);
	typeList->addItem(QTStr("Basic.Capture.Type.Display"), (int)CaptureTargetType::Display);
	typeList->addItem(QTStr("Basic.Capture.Type.Window"), (int)CaptureTargetType::Window);
	typeList->addItem(QTStr("Basic.Capture.Type.Game"), (int)CaptureTargetType::Game);

	RefreshingComboBox *refreshingTargetList = new RefreshingComboBox(this);
	refreshingTargetList->beforePopup = [this]() {
		RefreshTargets();
	};
	targetList = refreshingTargetList;
	targetList->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	targetList->setMinimumContentsLength(16);

	refreshButton = new QPushButton(this);
	refreshButton->setProperty("class", "icon-refresh");
	refreshButton->setToolTip(QTStr("Basic.Capture.Refresh"));

	QHBoxLayout *targetLayout = new QHBoxLayout();
	targetLayout->setContentsMargins(0, 0, 0, 0);
	targetLayout->addWidget(targetList, 1);
	targetLayout->addWidget(refreshButton);

	audioList = new QComboBox(this);
	audioList->addItem(QTStr("Basic.Capture.Audio.Desktop"), false);
	audioList->addItem(QTStr("Basic.Capture.Audio.App"), true);

	canvasLabel = new QLabel(this);

	settingsButton = new QPushButton(QTStr("Basic.Capture.Settings"), this);

	QFormLayout *layout = new QFormLayout(this);
	layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	layout->addRow(QTStr("Basic.Capture.Type"), typeList);
	layout->addRow(QTStr("Basic.Capture.Target"), targetLayout);
	layout->addRow(QTStr("Basic.Capture.Audio"), audioList);
	layout->addRow(QTStr("Basic.Capture.Canvas"), canvasLabel);
	layout->addRow(settingsButton);

	connect(typeList, &QComboBox::activated, this,
		[this]() { target->SetType((CaptureTargetType)typeList->currentData().toInt()); });
	connect(targetList, &QComboBox::activated, this,
		[this]() { target->SetTarget(targetList->currentData().toString()); });
	connect(refreshButton, &QPushButton::clicked, this, &CapturePanel::RefreshTargets);
	connect(audioList, &QComboBox::activated, this,
		[this]() { target->SetAppAudio(audioList->currentData().toBool()); });
	connect(settingsButton, &QPushButton::clicked, this, [this]() {
		if (OBSSource source = target->Source()) {
			main->CreatePropertiesWindow(source);
		}
	});

	connect(target, &CaptureTarget::TargetChanged, this, &CapturePanel::RefreshState);
	connect(target, &CaptureTarget::CanvasSizeChanged, this, &CapturePanel::UpdateCanvasLabel);
	connect(main, &OBSBasic::CanvasResized, this, &CapturePanel::UpdateCanvasLabel);

	RefreshState();
}

void CapturePanel::RefreshTargets()
{
	QSignalBlocker blocker(targetList);
	targetList->clear();

	QString current = target->TargetValue();
	bool found = false;

	for (const auto &[name, value] : target->Targets()) {
		targetList->addItem(name, value);
		found = found || value == current;
	}

	/* Keep a window that is closed right now selectable, it is captured again once it reappears */
	if (!found && !current.isEmpty()) {
		QString name = target->Type() == CaptureTargetType::Display ? QTStr("Basic.Capture.DisplayNotFound")
									    : DescribeWindowValue(current);
		targetList->addItem(QTStr("Basic.Capture.NotFound").arg(name), current);
	}

	targetList->setCurrentIndex(targetList->findData(current));
}

void CapturePanel::RefreshState()
{
	{
		QSignalBlocker blocker(typeList);
		typeList->setCurrentIndex(typeList->findData((int)target->Type()));
	}

	RefreshTargets();

	{
		QSignalBlocker blocker(audioList);
		bool appAudio = target->AppAudioSupported() && target->AppAudio();
		audioList->setCurrentIndex(audioList->findData(appAudio));
		audioList->setEnabled(target->AppAudioSupported());
	}

	UpdateCanvasLabel();
}

void CapturePanel::UpdateCanvasLabel()
{
	QSize size = target->CanvasSize();
	canvasLabel->setText(QString("%1 × %2").arg(size.width()).arg(size.height()));
}
