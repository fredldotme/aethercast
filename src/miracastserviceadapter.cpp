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

#include <glib.h>

#include "miracastserviceadapter.h"
#include "miracastservice.h"


MiracastServiceAdapter::MiracastServiceAdapter(MiracastService *service) :
    service_(service),
    manager_obj_(nullptr),
    bus_id_(0) {

    g_message("Created miracast service adapter");

    bus_id_ = g_bus_own_name(G_BUS_TYPE_SYSTEM, "org.freedesktop.miracast", G_BUS_NAME_OWNER_FLAGS_NONE,
                   nullptr, &MiracastServiceAdapter::OnNameAcquired, nullptr, this, nullptr);
    if (bus_id_ == 0) {
        g_warning("Failed to register bus name 'org.freedesktop.miracast'");
        return;
    }
}

MiracastServiceAdapter::~MiracastServiceAdapter() {
    if (bus_id_ > 0)
        g_bus_unown_name(bus_id_);
}

void MiracastServiceAdapter::OnNameAcquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    auto inst = static_cast<MiracastServiceAdapter*>(user_data);

    inst->manager_obj_ = miracast_interface_manager_skeleton_new();

    g_signal_connect(inst->manager_obj_, "handle-scan",
                     G_CALLBACK(&MiracastServiceAdapter::OnHandleScan), inst);
    g_signal_connect(inst->manager_obj_, "handle-connect-sink",
                     G_CALLBACK(&MiracastServiceAdapter::OnHandleConnectSink), inst);

    g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(inst->manager_obj_),
                                     connection, "/", nullptr);

    g_message("Registered bus name %s", name);
}

void MiracastServiceAdapter::OnHandleScan(MiracastInterfaceManager *skeleton,
                                        GDBusMethodInvocation *invocation, gpointer user_data) {

    auto inst = static_cast<MiracastServiceAdapter*>(user_data);

    g_message("Scanning for remote devices");

    inst->service_->Scan();

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

void MiracastServiceAdapter::OnHandleConnectSink(MiracastInterfaceManager *skeleton,
                                        GDBusMethodInvocation *invocation, const gchar *address, gpointer user_data) {
    auto inst = static_cast<MiracastServiceAdapter*>(user_data);

    inst->service_->ConnectSink(std::string(address), [=](bool success, const std::string &error_text) {
        if (!success) {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "%s", error_text.c_str());
            return;
        }

        g_dbus_method_invocation_return_value(invocation, nullptr);
    });

}
