#include "CaptureTarget.hpp"

#include <OBSApp.hpp>
#include <utility/platform.hpp>
#include <widgets/OBSBasic.hpp>

#include <QDateTime>

#include <algorithm>
#include <string>

#include "moc_CaptureTarget.cpp"

namespace {
constexpr int kPollIntervalMs = 500;
/* Number of consecutive polls a new size has to be seen before the canvas follows it. */
constexpr int kStableTicks = 2;
constexpr qint64 kRestartTimeoutMs = 15000;
constexpr int kRestartDelayMs = 250;
constexpr int kMinCanvasSize = 32;

constexpr const char *kDisplayCaptureId = "monitor_capture";
constexpr const char *kWindowCaptureId = "window_capture";
constexpr const char *kGameCaptureId = "game_capture";

const char *IdForType(CaptureTargetType type)
{
	switch (type) {
	case CaptureTargetType::Window:
		return kWindowCaptureId;
	case CaptureTargetType::Game:
		return kGameCaptureId;
	case CaptureTargetType::Display:
		break;
	}
	return kDisplayCaptureId;
}

bool TypeForId(const char *id, CaptureTargetType &type)
{
	if (strcmp(id, kDisplayCaptureId) == 0) {
		type = CaptureTargetType::Display;
	} else if (strcmp(id, kWindowCaptureId) == 0) {
		type = CaptureTargetType::Window;
	} else if (strcmp(id, kGameCaptureId) == 0) {
		type = CaptureTargetType::Game;
	} else {
		return false;
	}
	return true;
}

const char *TargetSettingName(CaptureTargetType type)
{
	return type == CaptureTargetType::Display ? "monitor_id" : "window";
}

/* Canvas dimensions for a captured size: width rounded down to a multiple of 4 and height to a multiple of 2, as
 * encoders expect, so the item can stay unscaled and only loses the remainder at the right and bottom edges. */
QSize CanvasSizeFor(uint32_t cx, uint32_t cy)
{
	return QSize(std::max((int)(cx & ~3u), kMinCanvasSize), std::max((int)(cy & ~1u), kMinCanvasSize));
}
} // namespace

CaptureTarget::CaptureTarget(OBSBasic *main_) : QObject(main_), main(main_)
{
	pollTimer.setInterval(kPollIntervalMs);
	connect(&pollTimer, &QTimer::timeout, this, &CaptureTarget::Poll);
}

void CaptureTarget::Attach()
{
	OBSScene scene = main->GetCurrentScene();
	if (!scene) {
		return;
	}

	/* Only the current scene is kept */
	std::vector<OBSSource> otherScenes;
	auto collectScenes = [](void *param, obs_source_t *sceneSource) {
		auto data = static_cast<std::pair<obs_scene_t *, std::vector<OBSSource> *> *>(param);
		if (obs_scene_from_source(sceneSource) != data->first) {
			data->second->emplace_back(sceneSource);
		}
		return true;
	};
	std::pair<obs_scene_t *, std::vector<OBSSource> *> sceneData{scene, &otherScenes};
	obs_enum_scenes(collectScenes, &sceneData);
	for (const OBSSource &other : otherScenes) {
		obs_source_remove(other);
	}

	/* Only the first capture source in it is kept */
	std::vector<OBSSceneItem> items;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *sceneItem, void *param) {
			static_cast<std::vector<OBSSceneItem> *>(param)->emplace_back(sceneItem);
			return true;
		},
		&items);

	OBSSceneItem keep;
	for (const OBSSceneItem &sceneItem : items) {
		CaptureTargetType itemType;
		obs_source_t *itemSource = obs_sceneitem_get_source(sceneItem);
		if (!keep && TypeForId(obs_source_get_id(itemSource), itemType)) {
			keep = sceneItem;
			type = itemType;
		} else {
			obs_sceneitem_remove(sceneItem);
		}
	}

	if (keep) {
		obs_sceneitem_set_locked(keep, true);
		weakSource = OBSGetWeakRef(obs_sceneitem_get_source(keep));
	} else {
		CreateSource(CaptureTargetType::Display, nullptr);
	}

	appAudio = config_get_bool(main->Config(), "Capture", "AppAudio");
	ApplyAudioMode();

	fitMode = -1;
	pendingTicks = 0;
	pollTimer.start();
	Poll();

	emit TargetChanged();
}

void CaptureTarget::SetType(CaptureTargetType newType)
{
	if (newType == type && Source()) {
		return;
	}

	CreateSource(newType, nullptr);
	ApplyAudioMode();
	main->SaveProject();

	emit TargetChanged();
}

CaptureTarget::TargetList CaptureTarget::Targets() const
{
	TargetList targets;

	if (type == CaptureTargetType::Game) {
		targets.emplace_back(QTStr("Basic.Capture.AnyFullscreen"), QString());
	}

	obs_properties_t *props = obs_get_source_properties(IdForType(type));
	obs_property_t *list = obs_properties_get(props, TargetSettingName(type));
	size_t count = obs_property_list_item_count(list);

	for (size_t i = 0; i < count; i++) {
		if (obs_property_list_item_disabled(list, i)) {
			continue;
		}
		targets.emplace_back(QT_UTF8(obs_property_list_item_name(list, i)),
				     QT_UTF8(obs_property_list_item_string(list, i)));
	}

	obs_properties_destroy(props);
	return targets;
}

QString CaptureTarget::TargetValue() const
{
	OBSSource source = Source();
	if (!source) {
		return QString();
	}

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	if (type == CaptureTargetType::Game && strcmp(obs_data_get_string(settings, "capture_mode"), "window") != 0) {
		return QString();
	}

	return QT_UTF8(obs_data_get_string(settings, TargetSettingName(type)));
}

void CaptureTarget::SetTarget(const QString &value)
{
	OBSSource source = Source();
	if (!source) {
		return;
	}

	OBSDataAutoRelease settings = obs_data_create();
	if (type == CaptureTargetType::Game) {
		obs_data_set_string(settings, "capture_mode", value.isEmpty() ? "any_fullscreen" : "window");
	}
	if (!value.isEmpty() || type != CaptureTargetType::Game) {
		obs_data_set_string(settings, TargetSettingName(type), QT_TO_UTF8(value));
	}

	obs_source_update(source, settings);
	main->SaveProject();

	emit TargetChanged();
}

void CaptureTarget::SetAppAudio(bool enable)
{
	if (enable == appAudio) {
		return;
	}

	appAudio = enable;
	config_set_bool(main->Config(), "Capture", "AppAudio", appAudio);
	config_save_safe(main->Config(), "tmp", nullptr);

	ApplyAudioMode();
	main->SaveProject();
}

QSize CaptureTarget::CanvasSize() const
{
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		return QSize();
	}
	return QSize((int)ovi.base_width, (int)ovi.base_height);
}

bool CaptureTarget::VideoMatches(const QSize &size) const
{
	/* The output is never scaled, so both the canvas and the output have to match */
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		return false;
	}
	return QSize((int)ovi.base_width, (int)ovi.base_height) == size &&
	       QSize((int)ovi.output_width, (int)ovi.output_height) == size;
}

void CaptureTarget::CreateSource(CaptureTargetType newType, obs_data_t *settings)
{
	OBSScene scene = main->GetCurrentScene();
	if (!scene) {
		return;
	}

	if (OBSSource oldSource = Source()) {
		obs_source_remove(oldSource);
	}
	weakSource = nullptr;
	type = newType;

	OBSDataAutoRelease defaults = obs_data_create();
	if (!settings) {
		settings = defaults;

		if (newType == CaptureTargetType::Game) {
			obs_data_set_string(settings, "capture_mode", "any_fullscreen");
		} else {
			std::string target;
#ifdef _WIN32
			if (newType == CaptureTargetType::Display) {
				target = GetPrimaryMonitorDeviceId();
			}
#endif
			if (target.empty()) {
				TargetList targets = Targets();
				if (!targets.empty()) {
					target = QT_TO_UTF8(targets.front().second);
				}
			}
			obs_data_set_string(settings, TargetSettingName(newType), target.c_str());
		}
	}

	OBSSourceAutoRelease source =
		obs_source_create(IdForType(newType), Str("Basic.Capture.SourceName"), settings, nullptr);
	if (!source) {
		blog(LOG_ERROR, "[Capture] Failed to create '%s' source", IdForType(newType));
		return;
	}

	obs_sceneitem_t *sceneItem = obs_scene_add(scene, source);
	obs_sceneitem_set_locked(sceneItem, true);

	weakSource = OBSGetWeakRef(source);
	fitMode = -1;
	pollTimer.start();

	blog(LOG_INFO, "[Capture] Capturing with '%s'", IdForType(newType));
}

void CaptureTarget::ApplyAudioMode()
{
	OBSSource source = Source();
	if (!source) {
		return;
	}

	bool useAppAudio = appAudio && AppAudioSupported();

	if (AppAudioSupported()) {
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_bool(settings, "capture_audio", useAppAudio);
		obs_source_update(source, settings);
	}

	/* Desktop audio would also contain the target app, so it is muted while the app's own audio is captured. A
	 * desktop source the user had muted already is left alone. */
	OBSSourceAutoRelease desktop = obs_get_output_source(1);
	bool mutedByApp = config_get_bool(main->Config(), "Capture", "DesktopMutedByApp");

	if (useAppAudio && !mutedByApp) {
		if (desktop && !obs_source_muted(desktop)) {
			obs_source_set_muted(desktop, true);
			mutedByApp = true;
		}
	} else if (!useAppAudio && mutedByApp) {
		if (desktop) {
			obs_source_set_muted(desktop, false);
		}
		mutedByApp = false;
	}

	config_set_bool(main->Config(), "Capture", "DesktopMutedByApp", mutedByApp);
	config_save_safe(main->Config(), "tmp", nullptr);
}

OBSSceneItem CaptureTarget::Item() const
{
	OBSScene scene = main->GetCurrentScene();
	OBSSource source = Source();
	if (!scene || !source) {
		return nullptr;
	}

	std::pair<obs_source_t *, OBSSceneItem> data{source, nullptr};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *sceneItem, void *param) {
			auto data = static_cast<std::pair<obs_source_t *, OBSSceneItem> *>(param);
			if (obs_sceneitem_get_source(sceneItem) == data->first) {
				data->second = sceneItem;
				return false;
			}
			return true;
		},
		&data);

	return data.second;
}

void CaptureTarget::FitItem(uint32_t sourceCX, uint32_t sourceCY)
{
	QSize canvas = CanvasSize();
	/* 0: unscaled at the top left corner, 1: scaled to fit while the canvas does not match the target yet */
	int mode = (sourceCX && sourceCY && CanvasSizeFor(sourceCX, sourceCY) == canvas) ? 0 : 1;
	if (mode == fitMode) {
		return;
	}

	OBSSceneItem sceneItem = Item();
	if (!sceneItem) {
		return;
	}

	obs_transform_info info = {};
	vec2_set(&info.pos, 0.0f, 0.0f);
	vec2_set(&info.scale, 1.0f, 1.0f);
	info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	info.bounds_type = mode == 0 ? OBS_BOUNDS_NONE : OBS_BOUNDS_SCALE_INNER;
	info.bounds_alignment = OBS_ALIGN_CENTER;
	vec2_set(&info.bounds, (float)canvas.width(), (float)canvas.height());

	obs_sceneitem_crop crop = {};
	obs_sceneitem_set_crop(sceneItem, &crop);
	obs_sceneitem_set_info2(sceneItem, &info);

	fitMode = mode;
}

void CaptureTarget::Poll()
{
	if (restartState == RestartState::Stopping) {
		ContinueRestart();
		return;
	}

	OBSSource source = Source();
	if (!source) {
		/* The scene collection is being cleared, Attach() starts polling again */
		pollTimer.stop();
		return;
	}

	uint32_t cx = obs_source_get_width(source);
	uint32_t cy = obs_source_get_height(source);
	FitItem(cx, cy);

	/* A minimized window or a game that is not hooked yet reports no size, keep the current canvas */
	if (!cx || !cy) {
		pendingTicks = 0;
		return;
	}

	QSize wanted = CanvasSizeFor(cx, cy);
	if (VideoMatches(wanted)) {
		pendingTicks = 0;
		return;
	}

	if (wanted != pendingSize) {
		pendingSize = wanted;
		pendingTicks = 0;
		return;
	}

	if (++pendingTicks < kStableTicks) {
		return;
	}
	pendingTicks = 0;

	if (main->Active() || obs_video_active()) {
		BeginRestart(wanted);
		return;
	}

	ApplyCanvasSize(wanted, cx, cy);
}

void CaptureTarget::SyncCanvasSize()
{
	OBSSource source = Source();
	if (!source || restartState != RestartState::Idle || main->Active() || obs_video_active()) {
		return;
	}

	uint32_t cx = obs_source_get_width(source);
	uint32_t cy = obs_source_get_height(source);
	if (!cx || !cy) {
		return;
	}

	QSize wanted = CanvasSizeFor(cx, cy);
	if (!VideoMatches(wanted)) {
		pendingTicks = 0;
		ApplyCanvasSize(wanted, cx, cy);
	}
}

void CaptureTarget::ApplyCanvasSize(const QSize &size, uint32_t sourceCX, uint32_t sourceCY)
{
	if (main->SetCanvasSize((uint32_t)size.width(), (uint32_t)size.height())) {
		fitMode = -1;
		FitItem(sourceCX, sourceCY);
		emit CanvasSizeChanged(size);
	}
}

void CaptureTarget::BeginRestart(const QSize &size)
{
	restartRecording = main->RecordingActive();
	restartReplayBuffer = main->ReplayBufferActive();
	restartVirtualCam = main->VirtualCamActive();

	/* Some other output keeps the video active; the canvas follows once it stops */
	if (!restartRecording && !restartReplayBuffer && !restartVirtualCam) {
		return;
	}

	blog(LOG_INFO, "[Capture] Target size changed to %dx%d, restarting outputs", size.width(), size.height());

	restartSize = size;
	restartState = RestartState::Stopping;
	restartDeadline = QDateTime::currentMSecsSinceEpoch() + kRestartTimeoutMs;
	emit RestartingChanged(true);

	main->ShowStatusBarMessage(QTStr("Basic.Capture.Restarting"));

	if (restartRecording) {
		main->StopRecording();
	}
	if (restartReplayBuffer) {
		main->StopReplayBuffer();
	}
	if (restartVirtualCam) {
		main->StopVirtualCam();
	}
}

void CaptureTarget::ContinueRestart()
{
	if (main->Active() || obs_video_active()) {
		if (QDateTime::currentMSecsSinceEpoch() > restartDeadline) {
			blog(LOG_WARNING, "[Capture] Outputs did not stop in time, not restarting them");
			restartState = RestartState::Idle;
			emit RestartingChanged(false);
		}
		return;
	}

	/* The target may have changed size again while the outputs were stopping */
	QSize wanted = restartSize;
	if (OBSSource source = Source()) {
		uint32_t cx = obs_source_get_width(source);
		uint32_t cy = obs_source_get_height(source);
		if (cx && cy) {
			wanted = CanvasSizeFor(cx, cy);
		}
	}

	if (main->SetCanvasSize((uint32_t)wanted.width(), (uint32_t)wanted.height())) {
		fitMode = -1;
		emit CanvasSizeChanged(wanted);
	}

	restartState = RestartState::Idle;
	QTimer::singleShot(kRestartDelayMs, this, &CaptureTarget::FinishRestart);
}

void CaptureTarget::FinishRestart()
{
	if (restartReplayBuffer) {
		main->StartReplayBuffer();
	}
	if (restartRecording) {
		main->StartRecording();
	}
	if (restartVirtualCam) {
		main->StartVirtualCam();
	}

	QSize canvas = CanvasSize();
	main->ShowStatusBarMessage(QTStr("Basic.Capture.Restarted").arg(canvas.width()).arg(canvas.height()));

	emit RestartingChanged(false);
}
