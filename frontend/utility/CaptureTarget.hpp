#pragma once

#include <obs.hpp>

#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>

#include <utility>
#include <vector>

class OBSBasic;

enum class CaptureTargetType {
	Display,
	Window,
	Game,
};

/* Owns the single capture source of the recorder: the program scene always contains exactly one display, window or
 * game capture source, and the canvas size follows the size of whatever that source captures. */
class CaptureTarget : public QObject {
	Q_OBJECT

public:
	using TargetList = std::vector<std::pair<QString, QString>>;

	explicit CaptureTarget(OBSBasic *main);

	/* Called after a scene collection has been loaded: reduces the collection to one scene with one capture
	 * source, creating a display capture of the primary monitor if there is none. */
	void Attach();

	CaptureTargetType Type() const { return type; }
	void SetType(CaptureTargetType newType);

	/* Display names and setting values of the targets available for the current type. For game capture, the
	 * empty value means "any fullscreen application". */
	TargetList Targets() const;
	QString TargetValue() const;
	void SetTarget(const QString &value);

	bool AppAudioSupported() const { return type != CaptureTargetType::Display; }
	bool AppAudio() const { return appAudio; }
	void SetAppAudio(bool enable);

	OBSSource Source() const { return OBSGetStrongRef(weakSource); }

	/* Applies a size change right away instead of waiting for it to settle, called before an output starts so
	 * it does not have to be restarted a moment later */
	void SyncCanvasSize();
	QSize CanvasSize() const;
	bool Restarting() const { return restartState != RestartState::Idle; }

signals:
	void TargetChanged();
	void CanvasSizeChanged(QSize size);
	void RestartingChanged(bool restarting);

private:
	enum class RestartState {
		Idle,
		Stopping,
	};

	OBSBasic *main;
	/* Weak, so clearing the scene collection is never blocked by a reference held here. */
	OBSWeakSource weakSource;
	CaptureTargetType type = CaptureTargetType::Display;
	bool appAudio = false;

	QTimer pollTimer;
	QSize pendingSize;
	int pendingTicks = 0;
	int fitMode = -1;

	RestartState restartState = RestartState::Idle;
	QSize restartSize;
	qint64 restartDeadline = 0;
	bool restartRecording = false;
	bool restartReplayBuffer = false;
	bool restartVirtualCam = false;

	bool VideoMatches(const QSize &size) const;
	void ApplyCanvasSize(const QSize &size, uint32_t sourceCX, uint32_t sourceCY);
	void CreateSource(CaptureTargetType newType, obs_data_t *settings);
	void ApplyAudioMode();
	OBSSceneItem Item() const;
	void FitItem(uint32_t sourceCX, uint32_t sourceCY);

	void Poll();
	void BeginRestart(const QSize &size);
	void ContinueRestart();
	void FinishRestart();
};
