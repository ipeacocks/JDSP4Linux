#include "PulseManager.h"

#include "Utils.h"
#include <glib.h>
#include <memory>

//#include <config/AppConfig.h>

PulseManager::PulseManager()
    : main_loop(pa_threaded_mainloop_new()), main_loop_api(pa_threaded_mainloop_get_api(main_loop)) {
    pa_threaded_mainloop_lock(main_loop);
    pa_threaded_mainloop_start(main_loop);

    /*std::vector<std::string> blocklist;
    for(const auto& block : AppConfig::instance().get<QStringList>(AppConfig::AudioAppBlocklist))
        blocklist.push_back(block.toStdString());
    blacklist_out = blocklist;*/

    context = pa_context_new(main_loop_api, SINK_NAME);

    pa_context_set_state_callback(context, &PulseManager::context_state_cb, this);
    pa_context_connect(context, nullptr, PA_CONTEXT_NOFAIL, nullptr);
    pa_threaded_mainloop_wait(main_loop);
    pa_threaded_mainloop_unlock(main_loop);

    if (context_ready) {
        get_server_info();
        load_apps_sink();
        subscribe_to_events();
    } else {
        util::error(log_tag + "context initialization failed");
    }
}

PulseManager::~PulseManager() {
    unload_sinks();

    drain_context();

    pa_threaded_mainloop_lock(main_loop);

    util::debug(log_tag + "disconnecting Pulseaudio context...");
    pa_context_disconnect(context);

    util::debug(log_tag + "Pulseaudio context was disconnected");

    util::debug(log_tag + "unreferencing Pulseaudio context");
    pa_context_unref(context);

    pa_threaded_mainloop_unlock(main_loop);

    util::debug(log_tag + "stopping pulseaudio threaded main loop");
    pa_threaded_mainloop_stop(main_loop);

    util::debug(log_tag + "freeing Pulseaudio threaded main loop");
    pa_threaded_mainloop_free(main_loop);
}

void PulseManager::context_state_cb(pa_context* ctx, void* data) {
    auto pm = static_cast<PulseManager*>(data);

    auto state = pa_context_get_state(ctx);

    if (state == PA_CONTEXT_UNCONNECTED) {
        util::debug(pm->log_tag + "context is unconnected");
    } else if (state == PA_CONTEXT_CONNECTING) {
        util::debug(pm->log_tag + "context is connecting");
    } else if (state == PA_CONTEXT_AUTHORIZING) {
        util::debug(pm->log_tag + "context is authorizing");
    } else if (state == PA_CONTEXT_SETTING_NAME) {
        util::debug(pm->log_tag + "context is setting name");
    } else if (state == PA_CONTEXT_READY) {
        util::debug(pm->log_tag + "context is ready");
        util::debug(pm->log_tag + "connected to: " + pa_context_get_server(ctx));

        auto protocol = std::to_string(pa_context_get_protocol_version(ctx));

        pm->server_info.protocol = protocol;

        util::debug(pm->log_tag + "protocol version: " + protocol);

        pm->context_ready = true;
        pa_threaded_mainloop_signal(pm->main_loop, 0);
    } else if (state == PA_CONTEXT_FAILED) {
        util::debug(pm->log_tag + "failed to connect context");

        pm->context_ready = false;
        pa_threaded_mainloop_signal(pm->main_loop, 0);
    } else if (state == PA_CONTEXT_TERMINATED) {
        util::debug(pm->log_tag + "context was terminated");

        pm->context_ready = false;
        pa_threaded_mainloop_signal(pm->main_loop, 0);
    }
}

void PulseManager::subscribe_to_events() {
    pa_context_set_subscribe_callback(
                context,
                [](auto c, auto t, auto idx, auto d) {
        auto f = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;

        auto pm = static_cast<PulseManager*>(d);

        if (f == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
            auto e = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

            if (e == PA_SUBSCRIPTION_EVENT_NEW) {
                pa_context_get_sink_input_info(
                            c, idx,
                            []([[maybe_unused]] auto cx, auto info,
                            [[maybe_unused]] auto eol, auto d) {
                    if (info != nullptr) {
                        auto pm = static_cast<PulseManager*>(d);
                        pm->new_app(info);
                    }
                },
                pm);
            } else if (e == PA_SUBSCRIPTION_EVENT_CHANGE) {
                pa_context_get_sink_input_info(
                            c, idx,
                            []([[maybe_unused]] auto cx, auto info,
                            [[maybe_unused]] auto eol, auto d) {
                    if (info != nullptr) {
                        auto pm = static_cast<PulseManager*>(d);
                        pm->changed_app(info);
                    }
                },
                pm);
            } else if (e == PA_SUBSCRIPTION_EVENT_REMOVE) {
                Glib::signal_idle().connect_once([pm, idx]() { pm->sink_input_removed(idx); });
            }
        } else if (f == PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT) {
            auto e = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

            if (e == PA_SUBSCRIPTION_EVENT_NEW) {
                pa_context_get_source_output_info(
                            c, idx,
                            []([[maybe_unused]] auto cx, auto info,
                            [[maybe_unused]] auto eol, auto d) {
                    if (info != nullptr) {
                        auto pm = static_cast<PulseManager*>(d);
                        pm->new_app(info);
                    }
                },
                pm);
            } else if (e == PA_SUBSCRIPTION_EVENT_CHANGE) {
                pa_context_get_source_output_info(
                            c, idx,
                            []([[maybe_unused]] auto cx, auto info,
                            [[maybe_unused]] auto eol, auto d) {
                    if (info != nullptr) {
                        auto pm = static_cast<PulseManager*>(d);
                        pm->changed_app(info);
                    }
                },
                pm);
            } else if (e == PA_SUBSCRIPTION_EVENT_REMOVE) {
                Glib::signal_idle().connect_once([pm, idx]() { pm->source_output_removed(idx); });
            }
        } else if (f == PA_SUBSCRIPTION_EVENT_SINK) {
            auto e = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

            if (e == PA_SUBSCRIPTION_EVENT_NEW) {
                pa_context_get_sink_info_by_index(
                            c, idx,
                            []([[maybe_unused]] auto cx, auto info,
                            [[maybe_unused]] auto eol, auto d) {
                    if (info != nullptr) {
                        if (std::strcmp(info->name, SINK_NAME) != 0) {
                            auto pm = static_cast<PulseManager*>(d);

                            auto si = std::make_shared<mySinkInfo>();

                            si->name = info->name;
                            si->index = info->index;
                            si->description = info->description;
                            si->rate = info->sample_spec.rate;
                            si->format = pa_sample_format_to_string(info->sample_spec.format);

                            if (info->active_port != nullptr) {
                                si->active_port = info->active_port->name;
                            } else {
                                si->active_port = "null";
                            }

                            Glib::signal_idle().connect_once([pm, si = move(si)] { pm->sink_added(si); });
                        }
                    }
                },
                pm);
            } else if (e == PA_SUBSCRIPTION_EVENT_CHANGE) {
                pa_context_get_sink_info_by_index(
                            c, idx,
                            []([[maybe_unused]] auto cx, auto info,
                            [[maybe_unused]] auto eol, auto d) {
                    if (info != nullptr) {
                        auto pm = static_cast<PulseManager*>(d);
                        auto si = std::make_shared<mySinkInfo>();

                        si->name = info->name;
                        si->index = info->index;
                        si->description = info->description;
                        si->rate = info->sample_spec.rate;
                        si->format = pa_sample_format_to_string(info->sample_spec.format);

                        if (info->active_port != nullptr) {
                            si->active_port = info->active_port->name;
                        } else {
                            si->active_port = "null";
                        }

                        if (si->name == SINK_NAME) {
                            pm->apps_sink_info->rate = si->rate;
                            pm->apps_sink_info->format = si->format;
                        }

                        Glib::signal_idle().connect_once([pm, si = move(si)] { pm->sink_changed(si); });
                    }
                },
                pm);
            } else if (e == PA_SUBSCRIPTION_EVENT_REMOVE) {
                Glib::signal_idle().connect_once([pm, idx]() { pm->sink_removed(idx); });
            }
        } else if (f == PA_SUBSCRIPTION_EVENT_SERVER) {
            auto e = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

            if (e == PA_SUBSCRIPTION_EVENT_CHANGE) {
                pa_context_get_server_info(
                            c,
                            []([[maybe_unused]] auto cx, auto info, auto d) {
                    if (info != nullptr) {
                        auto pm = static_cast<PulseManager*>(d);

                        pm->update_server_info(info);

                        std::string sink = info->default_sink_name;
                        std::string source = info->default_source_name;

                        if (sink != std::string(SINK_NAME)) {
                            Glib::signal_idle().connect_once([pm, sink]() { pm->new_default_sink(sink); });
                        }

                        Glib::signal_idle().connect_once([pm, source]() { pm->new_default_source(source); });

                        Glib::signal_idle().connect_once([pm]() { pm->server_changed(); });
                    }
                },
                pm);
            }
        }
    },
    this);

    auto mask = static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT |
                                                    PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SINK |
                                                    PA_SUBSCRIPTION_MASK_SERVER);

    pa_context_subscribe(
                context, mask,
                []([[maybe_unused]] auto c, auto success, auto d) {
        auto pm = static_cast<PulseManager*>(d);

        if (success == 0) {
            util::critical(pm->log_tag + "context event subscribe failed!");
        }
    },
    this);
}

void PulseManager::update_server_info(const pa_server_info* info) {
    server_info.server_name = info->server_name;
    server_info.server_version = info->server_version;
    server_info.default_sink_name = info->default_sink_name;
    server_info.default_source_name = info->default_source_name;
    server_info.format = pa_sample_format_to_string(info->sample_spec.format);
    server_info.rate = info->sample_spec.rate;
    server_info.channels = info->sample_spec.channels;

    if (pa_channel_map_to_pretty_name(&info->channel_map) != nullptr) {
        server_info.channel_map = pa_channel_map_to_pretty_name(&info->channel_map);
    } else {
        server_info.channel_map = "unknown";
    }
}

void PulseManager::get_server_info() {
    pa_threaded_mainloop_lock(main_loop);

    auto o = pa_context_get_server_info(
                context,
                []([[maybe_unused]] auto c, auto info, auto d) {
        auto pm = static_cast<PulseManager*>(d);

        if (info != nullptr) {
            pm->update_server_info(info);

            util::debug(pm->log_tag + "Pulseaudio version: " + info->server_version);
            util::debug(pm->log_tag + "default pulseaudio source: " + info->default_source_name);
            util::debug(pm->log_tag + "default pulseaudio sink: " + info->default_sink_name);
        }

        pa_threaded_mainloop_signal(pm->main_loop, false);
    },
    this);

    if (o != nullptr) {
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(main_loop);
        }

        pa_operation_unref(o);
    } else {
        util::critical(log_tag + " failed to get server info");
    }

    pa_threaded_mainloop_unlock(main_loop);
}

auto PulseManager::get_sink_info(const std::string& name) -> std::shared_ptr<mySinkInfo> {
    auto si = std::make_shared<mySinkInfo>();

    struct Data {
        bool failed;
        PulseManager* pm;
        std::shared_ptr<mySinkInfo> si;
    };

    Data data = {false, this, si};

    pa_threaded_mainloop_lock(main_loop);

    auto o = pa_context_get_sink_info_by_name(
                context, name.c_str(),
                []([[maybe_unused]] auto c, auto info, auto eol, auto data) {
        auto d = static_cast<Data*>(data);

        if (eol < 0) {
            d->failed = true;
            pa_threaded_mainloop_signal(d->pm->main_loop, false);
        } else if (eol > 0) {
            pa_threaded_mainloop_signal(d->pm->main_loop, false);
        } else if (info != nullptr) {
            d->si->name = info->name;
            d->si->index = info->index;
            d->si->description = info->description;
            d->si->owner_module = info->owner_module;
            d->si->monitor_source = info->monitor_source;
            d->si->monitor_source_name = info->monitor_source_name;
            d->si->rate = info->sample_spec.rate;
            d->si->format = pa_sample_format_to_string(info->sample_spec.format);

            if (info->active_port != nullptr) {
                d->si->active_port = info->active_port->name;
            } else {
                d->si->active_port = "null";
            }
        }
    },
    &data);

    if (o != nullptr) {
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(main_loop);
        }

        pa_operation_unref(o);
    } else {
        util::critical(log_tag + " failed to get sink info: " + name);
    }

    pa_threaded_mainloop_unlock(main_loop);

    if (!data.failed) {
        return si;
    }

    util::debug(log_tag + " failed to get sink info: " + name);

    return nullptr;
}

auto PulseManager::get_default_sink_info() -> std::shared_ptr<mySinkInfo> {
    auto info = get_sink_info(server_info.default_sink_name);

    if (info != nullptr) {
        util::debug(log_tag + "default pulseaudio sink sampling rate: " + std::to_string(info->rate) + " Hz");
        util::debug(log_tag + "default pulseaudio sink audio format: " + info->format);

        return info;
    }

    util::critical(log_tag + "could not get default sink info");

    return nullptr;
}

auto PulseManager::load_module(const std::string& name, const std::string& argument) -> bool {
    struct Data {
        bool status;
        PulseManager* pm;
    };

    Data data = {false, this};

    pa_threaded_mainloop_lock(main_loop);

    auto o = pa_context_load_module(
                context, name.c_str(), argument.c_str(),
                []([[maybe_unused]] auto c, auto idx, auto data) {
        auto d = static_cast<Data*>(data);

        d->status = idx != PA_INVALID_INDEX;

        pa_threaded_mainloop_signal(d->pm->main_loop, false);
    },
    &data);

    if (o != nullptr) {
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(main_loop);
        }

        pa_operation_unref(o);
    }

    pa_threaded_mainloop_unlock(main_loop);

    return data.status;
}

auto PulseManager::load_sink(const std::string& name, const std::string& description, uint rate)
-> std::shared_ptr<mySinkInfo> {
    auto si = get_sink_info(name);

    if (si == nullptr) {  // sink is not loaded
        std::string argument =
                "sink_name=" + name + " " + "sink_properties=" + description + "device.class=\"sound\"" + " " + "norewinds=1";

        bool ok = load_module("module-null-sink", argument);

        if (ok) {
            util::debug(log_tag + "loaded module-null-sink: " + argument);

            si = get_sink_info(name);
        } else {
            util::warning(
                        log_tag + "Pulseaudio " + server_info.server_version +
                        " does not support norewinds. Loading the sink the old way. Changing apps volume will cause cracklings");

            argument = "sink_name=" + name + " " + "sink_properties=" + description + "device.class=\"sound\"" + " " +
                    "channels=2" + " " + "rate=" + std::to_string(rate);

            ok = load_module("module-null-sink", argument);

            if (ok) {
                util::debug(log_tag + "loaded module-null-sink: " + argument);

                si = get_sink_info(name);
            } else {
                util::critical(log_tag + "failed to load module-null-sink with argument: " + argument);
            }
        }
    }

    return si;
}

void PulseManager::load_apps_sink() {
    util::debug(log_tag + "loading applications output sink...");

    auto info = get_default_sink_info();

    if (info != nullptr) {
        std::string name = SINK_NAME;
        std::string description = "device.description=\"" SINK_DESC "\"";
        auto rate = info->rate;

        apps_sink_info = load_sink(name, description, rate);
    }
}

void PulseManager::find_sink_inputs() {
    pa_threaded_mainloop_lock(main_loop);

    auto o = pa_context_get_sink_input_info_list(
                context,
                []([[maybe_unused]] auto c, auto info,
                [[maybe_unused]] auto eol, auto d) {
            auto pm = static_cast<PulseManager*>(d);

            if (info != nullptr) {
            pm->new_app(info);
} else {
            pa_threaded_mainloop_signal(pm->main_loop, false);
}
},
            this);

    if (o != nullptr) {
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(main_loop);
        }

        pa_operation_unref(o);
    } else {
        util::warning(log_tag + " failed to find sink inputs");
    }

    pa_threaded_mainloop_unlock(main_loop);
}

void PulseManager::find_source_outputs() {
    pa_threaded_mainloop_lock(main_loop);

    auto o = pa_context_get_source_output_info_list(
                context,
                []([[maybe_unused]] auto c, auto info,
                [[maybe_unused]] auto eol, auto d) {
            auto pm = static_cast<PulseManager*>(d);

            if (info != nullptr) {
            pm->new_app(info);
} else {
            pa_threaded_mainloop_signal(pm->main_loop, false);
}
},
            this);

    if (o != nullptr) {
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(main_loop);
        }

        pa_operation_unref(o);
    } else {
        util::warning(log_tag + " failed to find source outputs");
    }

    pa_threaded_mainloop_unlock(main_loop);
}

void PulseManager::find_sinks() {
    pa_threaded_mainloop_lock(main_loop);

    auto o = pa_context_get_sink_info_list(
                context,
                []([[maybe_unused]] auto c, auto info,
                [[maybe_unused]] auto eol, auto d) {
            auto pm = static_cast<PulseManager*>(d);

            if (info != nullptr) {
            if (std::strcmp(info->name, SINK_NAME) != 0) {
            auto si = std::make_shared<mySinkInfo>();

            si->name = info->name;
            si->index = info->index;
            si->description = info->description;
            si->rate = info->sample_spec.rate;
            si->format = pa_sample_format_to_string(info->sample_spec.format);

            Glib::signal_idle().connect_once([pm, si = move(si)] { pm->sink_added(si); });
}
} else {
            pa_threaded_mainloop_signal(pm->main_loop, false);
}
},
            this);

    if (o != nullptr) {
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(main_loop);
        }

        pa_operation_unref(o);
    } else {
        util::warning(log_tag + " failed to find sinks");
    }

    pa_threaded_mainloop_unlock(main_loop);
}

auto PulseManager::move_sink_input_to_gstmgr(const std::string& name, uint idx) -> bool {
    struct Data {
        std::string name;
        uint idx;
        PulseManager* pm;
    };

    Data data = {name, idx, this};

    bool added_successfully = false;

    pa_threaded_mainloop_lock(main_loop);

    auto o = pa_context_move_sink_input_by_index(
                context, idx, apps_sink_info->index,
                []([[maybe_unused]] auto c, auto success, auto data) {
        auto d = static_cast<Data*>(data);

        if (success) {
            util::debug(d->pm->log_tag + "sink input: " + d->name + ", idx = " + std::to_string(d->idx) + " moved to PE");
        } else {
            util::critical(d->pm->log_tag + "failed to move sink input: " + d->name +
                           ", idx = " + std::to_string(d->idx) + " to " SINK_NAME);
        }

        pa_threaded_mainloop_signal(d->pm->main_loop, false);
    },
    &data);

    if (o != nullptr) {
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(main_loop);
        }

        pa_operation_unref(o);

        added_successfully = true;
    } else {
        util::critical(log_tag + "failed to move sink input: " + name + ", idx = " + std::to_string(idx) + " to PE");
    }

    pa_threaded_mainloop_unlock(main_loop);

    return added_successfully;
}

auto PulseManager::remove_sink_input_from_gstmgr(const std::string& name, uint idx) -> bool {
    struct Data {
        std::string name;
        uint idx;
        PulseManager* pm;
    };

    Data data = {name, idx, this};

    bool removed_successfully = false;

    pa_threaded_mainloop_lock(main_loop);

    auto o = pa_context_move_sink_input_by_name(
                context, idx, server_info.default_sink_name.c_str(),
                []([[maybe_unused]] auto c, auto success, auto data) {
        auto d = static_cast<Data*>(data);

        if (success) {
            util::debug(d->pm->log_tag + "sink input: " + d->name + ", idx = " + std::to_string(d->idx) +
                        " removed from PE");
        } else {
            util::critical(d->pm->log_tag + "failed to remove sink input: " + d->name +
                           ", idx = " + std::to_string(d->idx) + " from PE");
        }

        pa_threaded_mainloop_signal(d->pm->main_loop, false);
    },
    &data);

    if (o != nullptr) {
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(main_loop);
        }

        pa_operation_unref(o);

        removed_successfully = true;
    } else {
        util::critical(log_tag + "failed to remove sink input: " + name + ", idx = " + std::to_string(idx) + " from PE");
    }

    pa_threaded_mainloop_unlock(main_loop);

    return removed_successfully;
}

void PulseManager::set_sink_input_volume(const std::string& name, uint idx, uint8_t channels, uint value) {
    pa_volume_t raw_value = PA_VOLUME_NORM * value / 100.0;

    auto cvol = pa_cvolume();

    auto cvol_ptr = pa_cvolume_set(&cvol, channels, raw_value);

    if (cvol_ptr != nullptr) {
        struct Data {
            std::string name;
            uint idx;
            PulseManager* pm;
        };

        Data data = {name, idx, this};

        pa_threaded_mainloop_lock(main_loop);

        auto o = pa_context_set_sink_input_volume(
                    context, idx, cvol_ptr,
                    []([[maybe_unused]] auto c, auto success, auto data) {
            auto d = static_cast<Data*>(data);

            if (success == 1) {
                util::debug(d->pm->log_tag + "changed volume of sink input: " + d->name +
                            ", idx = " + std::to_string(d->idx));
            } else {
                util::critical(d->pm->log_tag + "failed to change volume of sink input: " + d->name +
                               ", idx = " + std::to_string(d->idx));
            }

            pa_threaded_mainloop_signal(d->pm->main_loop, false);
        },
        &data);

        if (o != nullptr) {
            while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
                pa_threaded_mainloop_wait(main_loop);
            }

            pa_operation_unref(o);

            pa_threaded_mainloop_unlock(main_loop);
        } else {
            util::warning(log_tag + "failed to change volume of sink input: " + name + ", idx = " + std::to_string(idx));

            pa_threaded_mainloop_unlock(main_loop);
        }
    }
}

void PulseManager::set_sink_input_mute(const std::string& name, uint idx, bool state) {
    struct Data {
        std::string name;
        uint idx;
        PulseManager* pm;
    };

    Data data = {name, idx, this};

    pa_threaded_mainloop_lock(main_loop);

    auto o = pa_context_set_sink_input_mute(
                context, idx, static_cast<int>(state),
                []([[maybe_unused]] auto c, auto success, auto data) {
        auto d = static_cast<Data*>(data);

        if (success == 1) {
            util::debug(d->pm->log_tag + "sink input: " + d->name + ", idx = " + std::to_string(d->idx) + " is muted");
        } else {
            util::critical(d->pm->log_tag + "failed to mute sink input: " + d->name +
                           ", idx = " + std::to_string(d->idx));
        }

        pa_threaded_mainloop_signal(d->pm->main_loop, false);
    },
    &data);

    if (o != nullptr) {
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(main_loop);
        }

        pa_operation_unref(o);
    } else {
        util::warning(log_tag + "failed to mute set sink input: " + name + ", idx = " + std::to_string(idx) + " to PE");
    }

    pa_threaded_mainloop_unlock(main_loop);
}

void PulseManager::set_source_output_volume(const std::string& name, uint idx, uint8_t channels, uint value) {
    pa_volume_t raw_value = PA_VOLUME_NORM * value / 100.0;

    auto cvol = pa_cvolume();

    auto cvol_ptr = pa_cvolume_set(&cvol, channels, raw_value);

    if (cvol_ptr != nullptr) {
        struct Data {
            std::string name;
            uint idx;
            PulseManager* pm;
        };

        Data data = {name, idx, this};

        pa_threaded_mainloop_lock(main_loop);

        auto o = pa_context_set_source_output_volume(
                    context, idx, cvol_ptr,
                    []([[maybe_unused]] auto c, auto success, auto data) {
            auto d = static_cast<Data*>(data);

            if (success == 1) {
                util::debug(d->pm->log_tag + "changed volume of source output: " + d->name +
                            ", idx = " + std::to_string(d->idx));
            } else {
                util::debug(d->pm->log_tag + "failed to change volume of source output: " + d->name +
                            ", idx = " + std::to_string(d->idx));
            }

            pa_threaded_mainloop_signal(d->pm->main_loop, false);
        },
        &data);

        if (o != nullptr) {
            while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
                pa_threaded_mainloop_wait(main_loop);
            }

            pa_operation_unref(o);

            pa_threaded_mainloop_unlock(main_loop);
        } else {
            util::warning(log_tag + "failed to change volume of source output: " + name + ", idx = " + std::to_string(idx));

            pa_threaded_mainloop_unlock(main_loop);
        }
    }
}

void PulseManager::set_source_output_mute(const std::string& name, uint idx, bool state) {
    struct Data {
        std::string name;
        uint idx;
        PulseManager* pm;
    };

    Data data = {name, idx, this};

    pa_threaded_mainloop_lock(main_loop);

    auto o = pa_context_set_source_output_mute(
                context, idx, static_cast<int>(state),
                []([[maybe_unused]] auto c, auto success, auto data) {
        auto d = static_cast<Data*>(data);

        if (success == 1) {
            util::debug(d->pm->log_tag + "source output: " + d->name + ", idx = " + std::to_string(d->idx) + " is muted");
        } else {
            util::critical(d->pm->log_tag + "failed to mute source output: " + d->name +
                           ", idx = " + std::to_string(d->idx));
        }

        pa_threaded_mainloop_signal(d->pm->main_loop, false);
    },
    &data);

    if (o != nullptr) {
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(main_loop);
        }

        pa_operation_unref(o);
    } else {
        util::warning(log_tag + "failed to mute source output: " + name + ", idx = " + std::to_string(idx) + " to PE");
    }

    pa_threaded_mainloop_unlock(main_loop);
}

void PulseManager::get_sink_input_info(uint idx) {
    pa_threaded_mainloop_lock(main_loop);

    auto o = pa_context_get_sink_input_info(
                context, idx,
                []([[maybe_unused]] auto c, auto info,
                [[maybe_unused]] auto eol, auto d) {
            auto pm = static_cast<PulseManager*>(d);

            if (info != nullptr) {
            pm->changed_app(info);
} else {
            pa_threaded_mainloop_signal(pm->main_loop, false);
}
},
            this);

    if (o != nullptr) {
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(main_loop);
        }

        pa_operation_unref(o);
    } else {
        util::critical(log_tag + "failed to get sink input info: " + std::to_string(idx));
    }

    pa_threaded_mainloop_unlock(main_loop);
}

void PulseManager::get_modules_info() {
    pa_threaded_mainloop_lock(main_loop);

    auto o = pa_context_get_module_info_list(
                context,
                []([[maybe_unused]] auto c, auto info,
                [[maybe_unused]] auto eol, auto d) {
            auto pm = static_cast<PulseManager*>(d);

            if (info != nullptr) {
            auto mi = std::make_shared<myModuleInfo>();

            if (info->name) {
            mi->name = info->name;
            mi->index = info->index;

            if (info->argument) {
            mi->argument = info->argument;
} else {
            mi->argument = "";
}

            Glib::signal_idle().connect_once([pm, mi = move(mi)] { pm->module_info(mi); });
}
} else {
            pa_threaded_mainloop_signal(pm->main_loop, false);
}
},
            this);

    if (o != nullptr) {
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(main_loop);
        }

        pa_operation_unref(o);
    } else {
        util::critical(log_tag + "failed to get modules info");
    }

    pa_threaded_mainloop_unlock(main_loop);
}

void PulseManager::get_clients_info() {
    pa_threaded_mainloop_lock(main_loop);

    auto o = pa_context_get_client_info_list(
                context,
                []([[maybe_unused]] auto c, auto info,
                [[maybe_unused]] auto eol, auto d) {
            auto pm = static_cast<PulseManager*>(d);

            if (info != nullptr) {
            auto mi = std::make_shared<myClientInfo>();

            if (info->name) {
            mi->name = info->name;
            mi->index = info->index;

            if (pa_proplist_contains(info->proplist, "application.process.binary") == 1) {
            mi->binary = pa_proplist_gets(info->proplist, "application.process.binary");
} else {
            mi->binary = "";
}

            Glib::signal_idle().connect_once([pm, mi = move(mi)] { pm->client_info(mi); });
}
} else {
            pa_threaded_mainloop_signal(pm->main_loop, false);
}
},
            this);

    if (o != nullptr) {
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(main_loop);
        }

        pa_operation_unref(o);
    } else {
        util::critical(log_tag + "failed to get clients info");
    }

    pa_threaded_mainloop_unlock(main_loop);
}

void PulseManager::unload_module(uint idx) {
    struct Data {
        uint idx;
        PulseManager* pm;
    };

    Data data = {idx, this};

    pa_threaded_mainloop_lock(main_loop);

    auto o = pa_context_unload_module(
                context, idx,
                []([[maybe_unused]] auto c, auto success, auto data) {
        auto d = static_cast<Data*>(data);

        if (success) {
            util::debug(d->pm->log_tag + "module " + std::to_string(d->idx) + " unloaded");
        } else {
            util::critical(d->pm->log_tag + "failed to unload module " + std::to_string(d->idx));
        }

        pa_threaded_mainloop_signal(d->pm->main_loop, false);
    },
    &data);

    if (o != nullptr) {
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(main_loop);
        }

        pa_operation_unref(o);
    } else {
        util::critical(log_tag + "failed to unload module: " + std::to_string(idx));
    }

    pa_threaded_mainloop_unlock(main_loop);
}

void PulseManager::unload_sinks() {
    util::debug(log_tag + "unloading JamesDSP sinks...");

    unload_module(apps_sink_info->owner_module);
}

void PulseManager::drain_context() {
    pa_threaded_mainloop_lock(main_loop);

    auto o = pa_context_drain(
                context,
                [](auto c, auto d) {
        auto pm = static_cast<PulseManager*>(d);

        if (pa_context_get_state(c) == PA_CONTEXT_READY) {
            pa_threaded_mainloop_signal(pm->main_loop, false);
        }
    },
    this);

    if (o != nullptr) {
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(main_loop);
        }

        pa_operation_unref(o);

        pa_threaded_mainloop_unlock(main_loop);

        util::debug(log_tag + "Context was drained");
    } else {
        pa_threaded_mainloop_unlock(main_loop);

        util::debug(log_tag + "Context did not need draining");
    }
}

void PulseManager::new_app(const pa_sink_input_info* info) {
    auto app_info = parse_app_info(info);

    if (app_info != nullptr) {
        app_info->app_type = "sink_input";

        Glib::signal_idle().connect_once([&, app_info = move(app_info)]() { sink_input_added(app_info); });
    }
}

void PulseManager::new_app(const pa_source_output_info* info) {
    auto app_info = parse_app_info(info);

    if (app_info != nullptr) {
        app_info->app_type = "source_output";

        Glib::signal_idle().connect_once([&, app_info = move(app_info)]() { source_output_added(app_info); });
    }
}

void PulseManager::changed_app(const pa_sink_input_info* info) {
    auto app_info = parse_app_info(info);

    if (app_info != nullptr) {
        // checking if the user blacklisted this app

        auto forbidden_app =
                std::find(std::begin(blacklist_out), std::end(blacklist_out), app_info->name) != std::end(blacklist_out);

        if (!forbidden_app) {
            app_info->app_type = "sink_input";

            Glib::signal_idle().connect_once([&, app_info = move(app_info)]() { sink_input_changed(app_info); });
        }
    }
}

void PulseManager::changed_app(const pa_source_output_info* info) {
    auto app_info = parse_app_info(info);

    if (app_info != nullptr) {
        // checking if the user blacklisted this app

        auto forbidden_app =
                std::find(std::begin(blacklist_in), std::end(blacklist_in), app_info->name) != std::end(blacklist_in);

        if (!forbidden_app) {
            app_info->app_type = "source_output";

            Glib::signal_idle().connect_once([&, app_info = move(app_info)]() { source_output_changed(app_info); });
        }
    }
}

void PulseManager::print_app_info(const std::shared_ptr<AppInfo>& info) {
    std::cout << "index: " << info->index << std::endl;
    std::cout << "name: " << info->name << std::endl;
    std::cout << "icon name: " << info->icon_name << std::endl;
    std::cout << "channels: " << info->channels << std::endl;
    std::cout << "volume: " << info->volume << std::endl;
    std::cout << "rate: " << info->rate << std::endl;
    std::cout << "resampler: " << info->resampler << std::endl;
    std::cout << "format: " << info->format << std::endl;
    std::cout << "wants to play: " << info->wants_to_play << std::endl;
}

auto PulseManager::app_is_connected(const pa_sink_input_info* info) -> bool {
    return info->sink == apps_sink_info->index;
}

/* STUB */
auto PulseManager::app_is_connected([[maybe_unused]] const pa_source_output_info* info) -> bool {
    return false;//;info->source == mic_sink_info->monitor_source;
}
