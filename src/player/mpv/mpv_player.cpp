#include "player/mpv/mpv_player.h"
#include <mpv/client.h>
#include <mpv/render.h>
#include <clocale>
#include <cmath>
#include "logging.h"

MpvPlayer::MpvPlayer() = default;

MpvPlayer::~MpvPlayer() {
    cleanup();
}

void MpvPlayer::cleanup() {
    if (render_ctx_) {
        mpv_render_context_free(render_ctx_);
        render_ctx_ = nullptr;
    }
    if (mpv_) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
    }
}

void MpvPlayer::onMpvRedraw(void* ctx) {
    MpvPlayer* player = static_cast<MpvPlayer*>(ctx);
    player->needs_redraw_ = true;
    if (player->redraw_callback_) {
        player->redraw_callback_();
    }
}

void MpvPlayer::onMpvWakeup(void* ctx) {
    MpvPlayer* player = static_cast<MpvPlayer*>(ctx);
    player->has_events_ = true;
    if (player->on_wakeup_) player->on_wakeup_();
}

void MpvPlayer::processEvents() {
    if (!mpv_ || !has_events_.exchange(false)) return;

    while (true) {
        mpv_event* event = mpv_wait_event(mpv_, 0);
        if (event->event_id == MPV_EVENT_NONE) break;
        handleMpvEvent(event);
    }
}

void MpvPlayer::handleMpvEvent(mpv_event* event) {
    switch (event->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            mpv_event_property* prop = static_cast<mpv_event_property*>(event->data);
            if (strcmp(prop->name, "playback-time") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                double pos = *static_cast<double*>(prop->data);
                // Filter jitter (15ms threshold like jellyfin-desktop)
                if (std::fabs(pos - last_position_) > 0.015) {
                    last_position_ = pos;
                    if (on_position_) on_position_(pos * 1000.0);
                }
            } else if (strcmp(prop->name, "duration") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                double dur = *static_cast<double*>(prop->data);
                if (on_duration_) on_duration_(dur * 1000.0);
            } else if (strcmp(prop->name, "pause") == 0 && prop->format == MPV_FORMAT_FLAG) {
                bool paused = *static_cast<int*>(prop->data) != 0;
                if (on_state_) on_state_(paused);
            } else if (strcmp(prop->name, "seeking") == 0 && prop->format == MPV_FORMAT_FLAG) {
                bool seeking = *static_cast<int*>(prop->data) != 0;
                if (seeking_ && !seeking) {
                    if (on_seeked_) on_seeked_(last_position_ * 1000.0);
                }
                seeking_ = seeking;
            } else if (strcmp(prop->name, "paused-for-cache") == 0 && prop->format == MPV_FORMAT_FLAG) {
                bool buffering = *static_cast<int*>(prop->data) != 0;
                if (on_buffering_) on_buffering_(buffering, last_position_ * 1000.0);
            } else if (strcmp(prop->name, "core-idle") == 0 && prop->format == MPV_FORMAT_FLAG) {
                bool idle = *static_cast<int*>(prop->data) != 0;
                if (on_core_idle_) on_core_idle_(idle, last_position_ * 1000.0);
            } else if (strcmp(prop->name, "eof-reached") == 0 && prop->format == MPV_FORMAT_FLAG) {
                bool eof = *static_cast<int*>(prop->data) != 0;
                if (eof && playing_) {
                    LOG_DEBUG(LOG_MPV, "eof-reached=true, track ended naturally");
                    playing_ = false;
                    if (on_finished_) on_finished_();
                }
            } else if (strcmp(prop->name, "demuxer-cache-state") == 0 && prop->format == MPV_FORMAT_NODE) {
                if (on_buffered_ranges_) {
                    std::vector<BufferedRange> ranges;
                    mpv_node* node = static_cast<mpv_node*>(prop->data);
                    if (node && node->format == MPV_FORMAT_NODE_MAP) {
                        for (int i = 0; i < node->u.list->num; i++) {
                            if (strcmp(node->u.list->keys[i], "seekable-ranges") == 0) {
                                mpv_node* arr = &node->u.list->values[i];
                                if (arr->format == MPV_FORMAT_NODE_ARRAY) {
                                    for (int j = 0; j < arr->u.list->num; j++) {
                                        mpv_node* range = &arr->u.list->values[j];
                                        if (range->format == MPV_FORMAT_NODE_MAP) {
                                            double start = 0, end = 0;
                                            for (int k = 0; k < range->u.list->num; k++) {
                                                if (strcmp(range->u.list->keys[k], "start") == 0 && range->u.list->values[k].format == MPV_FORMAT_DOUBLE)
                                                    start = range->u.list->values[k].u.double_;
                                                else if (strcmp(range->u.list->keys[k], "end") == 0 && range->u.list->values[k].format == MPV_FORMAT_DOUBLE)
                                                    end = range->u.list->values[k].u.double_;
                                            }
                                            // Convert seconds to ticks (100ns units)
                                            ranges.push_back({
                                                static_cast<int64_t>(start * 10000000.0),
                                                static_cast<int64_t>(end * 10000000.0)
                                            });
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                    on_buffered_ranges_(ranges);
                }
            }
            break;
        }
        case MPV_EVENT_START_FILE:
            playing_ = true;
            break;
        case MPV_EVENT_FILE_LOADED:
            if (on_playing_) on_playing_();
            break;
        case MPV_EVENT_END_FILE: {
            mpv_event_end_file* ef = static_cast<mpv_event_end_file*>(event->data);
            LOG_DEBUG(LOG_MPV, "END_FILE reason=%d (0=EOF, 2=STOP, 4=ERROR)", ef->reason);
            // With keep-open=yes, EOF reason won't fire (handled by eof-reached property)
            // STOP reason fires on explicit stop command
            if (ef->reason == MPV_END_FILE_REASON_STOP) {
                playing_ = false;
                if (on_canceled_) on_canceled_();
            } else if (ef->reason == MPV_END_FILE_REASON_ERROR) {
                playing_ = false;
                std::string error = mpv_error_string(ef->error);
                LOG_ERROR(LOG_MPV, "Playback error: %s", error.c_str());
                if (on_error_) on_error_(error);
            }
            // Note: EOF/QUIT/REDIRECT reasons are handled by eof-reached property observation
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            auto* msg = static_cast<mpv_event_log_message*>(event->data);
            // Strip trailing newline if present
            std::string text = msg->text;
            if (!text.empty() && text.back() == '\n') {
                text.pop_back();
            }
            LOG_DEBUG(LOG_MPV, "%s: %s", msg->prefix, text.c_str());
            break;
        }
        // Async reply events (no action needed)
        case MPV_EVENT_COMMAND_REPLY:
        case MPV_EVENT_SET_PROPERTY_REPLY:
        // Stream reconfiguration notifications (handled internally by mpv)
        case MPV_EVENT_AUDIO_RECONFIG:
        case MPV_EVENT_VIDEO_RECONFIG:
        // Playback state transitions we don't need to act on
        case MPV_EVENT_SEEK:
        case MPV_EVENT_PLAYBACK_RESTART:
        case MPV_EVENT_IDLE:
            break;
        default:
            LOG_WARN(LOG_MPV, "Unexpected event: %s", mpv_event_name(event->event_id));
            break;
    }
}

bool MpvPlayer::init(const char* hwdec, PreInitHook preInitHook) {
    std::setlocale(LC_NUMERIC, "C");

    mpv_ = mpv_create();
    if (!mpv_) {
        LOG_ERROR(LOG_MPV, "mpv_create failed");
        return false;
    }

    mpv_set_option_string(mpv_, "vo", "libmpv");
    mpv_set_option_string(mpv_, "hwdec", hwdec);
    mpv_set_option_string(mpv_, "keep-open", "yes");  // Keep video layer alive, detect EOF via eof-reached property
    mpv_set_option_string(mpv_, "terminal", "no");
    mpv_set_option_string(mpv_, "video-sync", "audio");  // Simple audio sync, no frame interpolation
    mpv_set_option_string(mpv_, "interpolation", "no");  // Disable motion interpolation
    mpv_set_option_string(mpv_, "ytdl", "no");  // Disable youtube-dl
    // Discard audio output if no audio device could be opened
    // Prevents blocking/crashes on audio errors (like jellyfin-desktop)
    mpv_set_option_string(mpv_, "audio-fallback-to-null", "yes");

    if (preInitHook) {
        preInitHook(mpv_);
    }

    if (mpv_initialize(mpv_) < 0) {
        LOG_ERROR(LOG_MPV, "mpv_initialize failed");
        return false;
    }

    // Enable mpv log forwarding (info level by default)
    mpv_request_log_messages(mpv_, "info");

    // Set up property observation (like jellyfin-desktop)
    mpv_observe_property(mpv_, 0, "playback-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 0, "seeking", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 0, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 0, "core-idle", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, 0, "eof-reached", MPV_FORMAT_FLAG);  // Detect natural track end with keep-open=yes
    mpv_observe_property(mpv_, 0, "demuxer-cache-state", MPV_FORMAT_NODE);

    // Wakeup callback for event-driven processing
    mpv_set_wakeup_callback(mpv_, onMpvWakeup, this);

    return true;
}

bool MpvPlayer::createRenderContext(mpv_render_param* params) {
    int result = mpv_render_context_create(&render_ctx_, mpv_, params);
    if (result < 0) {
        LOG_ERROR(LOG_MPV, "mpv_render_context_create failed: %s", mpv_error_string(result));
        return false;
    }

    mpv_render_context_set_update_callback(render_ctx_, onMpvRedraw, this);
    return true;
}

bool MpvPlayer::loadFile(const std::string& path, double startSeconds) {
    // Clear pause state before loading
    int pause = 0;
    mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &pause);

    // Build command node: {"name": "loadfile", "url": path, "options": {"start": time}}
    std::string startStr = std::to_string(startSeconds);

    // Options map: {"start": "123.45"}
    mpv_node optVals[1] = {};
    optVals[0].u.string = const_cast<char*>(startStr.c_str());
    optVals[0].format = MPV_FORMAT_STRING;
    char* optKeyStrs[1] = {const_cast<char*>("start")};
    mpv_node_list optList = {1, optVals, optKeyStrs};
    mpv_node optNode = {};
    optNode.u.list = &optList;
    optNode.format = MPV_FORMAT_NODE_MAP;

    // Command map: {"name": "loadfile", "url": path, "options": {...}}
    mpv_node cmdVals[3] = {};
    cmdVals[0].u.string = const_cast<char*>("loadfile");
    cmdVals[0].format = MPV_FORMAT_STRING;
    cmdVals[1].u.string = const_cast<char*>(path.c_str());
    cmdVals[1].format = MPV_FORMAT_STRING;
    cmdVals[2] = optNode;
    char* cmdKeys[3] = {const_cast<char*>("name"), const_cast<char*>("url"), const_cast<char*>("options")};
    mpv_node_list cmdList = {3, cmdVals, cmdKeys};
    mpv_node cmdNode = {};
    cmdNode.u.list = &cmdList;
    cmdNode.format = MPV_FORMAT_NODE_MAP;

    int ret = mpv_command_node_async(mpv_, 0, &cmdNode);
    if (ret >= 0) {
        playing_ = true;
    } else {
        LOG_ERROR(LOG_MPV, "loadFile async failed: %s", mpv_error_string(ret));
    }
    return ret >= 0;
}

void MpvPlayer::stop() {
    if (!mpv_) return;
    const char* cmd[] = {"stop", nullptr};
    mpv_command_async(mpv_, 0, cmd);
    playing_ = false;
}

void MpvPlayer::pause() {
    if (!mpv_) return;
    int pause = 1;
    mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &pause);
}

void MpvPlayer::play() {
    if (!mpv_) return;
    int pause = 0;
    mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &pause);
}

void MpvPlayer::seek(double seconds) {
    if (!mpv_) return;
    std::string time_str = std::to_string(seconds);
    const char* cmd[] = {"seek", time_str.c_str(), "absolute", nullptr};
    mpv_command_async(mpv_, 0, cmd);
}

void MpvPlayer::setVolume(int volume) {
    if (!mpv_) return;
    double vol = static_cast<double>(volume);
    mpv_set_property_async(mpv_, 0, "volume", MPV_FORMAT_DOUBLE, &vol);
}

void MpvPlayer::setMuted(bool muted) {
    if (!mpv_) return;
    int m = muted ? 1 : 0;
    mpv_set_property_async(mpv_, 0, "mute", MPV_FORMAT_FLAG, &m);
}

void MpvPlayer::setSpeed(double speed) {
    if (!mpv_) return;
    mpv_set_property_async(mpv_, 0, "speed", MPV_FORMAT_DOUBLE, &speed);
}

void MpvPlayer::setNormalizationGain(double gainDb) {
    if (!mpv_) return;
    if (gainDb == 0.0) {
        // Clear audio filter
        mpv_set_property_string(mpv_, "af", "");
    } else {
        // Apply gain via lavfi volume filter
        // Format: lavfi=[volume=<dB>dB]
        char filter[64];
        snprintf(filter, sizeof(filter), "lavfi=[volume=%.2fdB]", gainDb);
        mpv_set_property_string(mpv_, "af", filter);
        LOG_INFO(LOG_MPV, "Normalization gain: %.2f dB", gainDb);
    }
}

void MpvPlayer::setSubtitleTrack(int sid) {
    if (!mpv_) return;
    if (sid < 0) {
        mpv_set_property_string(mpv_, "sid", "no");
    } else {
        int64_t id = sid;
        mpv_set_property_async(mpv_, 0, "sid", MPV_FORMAT_INT64, &id);
    }
}

void MpvPlayer::setAudioTrack(int aid) {
    if (!mpv_) return;
    if (aid < 0) {
        mpv_set_property_string(mpv_, "aid", "no");
    } else {
        int64_t id = aid;
        mpv_set_property_async(mpv_, 0, "aid", MPV_FORMAT_INT64, &id);
    }
}

void MpvPlayer::setAudioDelay(double seconds) {
    if (!mpv_) return;
    mpv_set_property_async(mpv_, 0, "audio-delay", MPV_FORMAT_DOUBLE, &seconds);
}

double MpvPlayer::getPosition() const {
    if (!mpv_) return 0;
    double pos = 0;
    mpv_get_property(mpv_, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    return pos;
}

double MpvPlayer::getSpeed() const {
    if (!mpv_) return 1.0;
    double speed = 1.0;
    mpv_get_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &speed);
    return speed;
}

double MpvPlayer::getDuration() const {
    if (!mpv_) return 0;
    double dur = 0;
    mpv_get_property(mpv_, "duration", MPV_FORMAT_DOUBLE, &dur);
    return dur;
}

bool MpvPlayer::isPaused() const {
    if (!mpv_) return false;
    int paused = 0;
    mpv_get_property(mpv_, "pause", MPV_FORMAT_FLAG, &paused);
    return paused != 0;
}

bool MpvPlayer::hasFrame() const {
    if (!render_ctx_) return false;
    uint64_t flags = mpv_render_context_update(render_ctx_);
    return (flags & MPV_RENDER_UPDATE_FRAME) != 0;
}

void MpvPlayer::reportSwap() {
    if (render_ctx_)
        mpv_render_context_report_swap(render_ctx_);
}
