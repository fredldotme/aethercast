/*
 * Copyright (C) 2015 Canonical, Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <boost/concept_check.hpp>

#include "mcs/logger.h"
#include "mcs/mir/screencast.h"

namespace {
static constexpr const char *kMirSocket{"/run/mir_socket"};
static constexpr const char *kMirConnectionName{"aethercast screencast client"};
}

namespace mcs {
namespace mir {

Screencast::Screencast() :
    connection_(nullptr),
    screencast_(nullptr),
    buffer_stream_(nullptr) {
}

Screencast::~Screencast() {
    if (screencast_)
        mir_screencast_release_sync(screencast_);

    if (connection_)
        mir_connection_release(connection_);
}

bool Screencast::Setup(const video::DisplayOutput &output) {
    if (screencast_ || connection_ || buffer_stream_)
        return false;

    if (output.mode != video::DisplayOutput::Mode::kExtend) {
        MCS_ERROR("Unsupported display output mode specified '%s'", output.mode);
        return false;
    }

    connection_ = mir_connect_sync(kMirSocket, kMirConnectionName);
    if (!mir_connection_is_valid(connection_)) {
        MCS_ERROR("Failed to connect to Mir server: %s",
                  mir_connection_get_error_message(connection_));
        return false;
    }

    const auto config = mir_connection_create_display_config(connection_);
    if (!config) {
        MCS_ERROR("Failed to create display configuration: %s",
                  mir_connection_get_error_message(connection_));
        return false;
    }

    MirDisplayOutput *active_output = nullptr;
    unsigned int output_index = 0;

    for (unsigned int i = 0; i < config->num_outputs; ++i) {
        if (config->outputs[i].connected &&
            config->outputs[i].used &&
            config->outputs[i].current_mode < config->outputs[i].num_modes) {
            // Found an active connection we can just use for our purpose
            active_output = &config->outputs[i];
            output_index = i;
            break;
        }
    }

    if (!active_output) {
        MCS_ERROR("Failed to find a suitable display output");
        return false;
    }

    const MirDisplayMode *display_mode = &active_output->modes[active_output->current_mode];

    params_.height = display_mode->vertical_resolution;
    params_.width = display_mode->horizontal_resolution;

    // If we request a screen region outside the available screen area
    // mir will create a mir output which is then available for everyone
    // as just another display.
    params_.region.left = params_.width;
    params_.region.top = 0;
    params_.region.width = output.width;
    params_.region.height = output.height;

    params_.width = output.width;
    params_.height = output.height;

    output_.refresh_rate = display_mode->refresh_rate;

    MCS_INFO("Selected output ID %i [(%ix%i)+(%ix%i)] orientation %d",
             output_index,
             params_.width, params_.height,
             params_.region.left, params_.region.top,
             active_output->orientation);

    MCS_DEBUG("Setting up screencast [%s %dx%d]", output.mode,
              output.width, output.height);

    unsigned int num_pixel_formats = 0;
    mir_connection_get_available_surface_formats(connection_, &params_.pixel_format,
                                                 1, &num_pixel_formats);
    if (num_pixel_formats == 0) {
        MCS_ERROR("Failed to find suitable pixel format: %s",
                  mir_connection_get_error_message(connection_));
        return false;
    }

    screencast_ = mir_connection_create_screencast_sync(connection_, &params_);
    if (!screencast_) {
        MCS_ERROR("Failed to create Mir screencast: %s",
                  mir_connection_get_error_message(connection_));
        return false;
    }

    buffer_stream_ = mir_screencast_get_buffer_stream(screencast_);
    if (!buffer_stream_) {
        MCS_ERROR("Failed to setup Mir buffer stream");
        return false;
    }

    output_ = output;

    return true;
}

void Screencast::SwapBuffers() {
    if (!buffer_stream_)
        return;

    mir_buffer_stream_swap_buffers_sync(buffer_stream_);
}

video::DisplayOutput Screencast::OutputMode() const {
    return output_;
}

void* Screencast::CurrentBuffer() const {
    if (!buffer_stream_)
        return nullptr;

    MirNativeBuffer *buffer = nullptr;
    mir_buffer_stream_get_current_buffer(buffer_stream_, &buffer);
    return reinterpret_cast<void*>(buffer);
}
} // namespace mir
} // namespace mcs