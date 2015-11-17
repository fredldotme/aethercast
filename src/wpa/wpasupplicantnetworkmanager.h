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

#ifndef NETWORKP2PMANAGERWPASUPPLICANT_H_
#define NETWORKP2PMANAGERWPASUPPLICANT_H_

#include <functional>
#include <memory>
#include <map>

#include <glib.h>

#include "networkdevice.h"
#include "networkmanager.h"

#include "dhcpclient.h"
#include "dhcpserver.h"

#include "wpasupplicantmessage.h"
#include "wpasupplicantcommandqueue.h"

class WpaSupplicantNetworkManager : public NetworkManager,
                                    public WpaSupplicantCommandQueue::Delegate,
                                    public DhcpClient::Delegate {
public:
    WpaSupplicantNetworkManager(NetworkManager::Delegate *delegate_, const std::string &iface);
    ~WpaSupplicantNetworkManager();

    void Setup();

    void SetWfdSubElements(const std::list<std::string> &elements) override;

    void Scan(unsigned int timeout = 30) override;
    std::vector<NetworkDevice::Ptr> Devices() const override;

    int Connect(const std::string &address, bool persistent = true) override;
    int DisconnectAll() override;

    NetworkDeviceRole Role() const override;
    std::string LocalAddress() const override;

    void OnUnsolicitedResponse(WpaSupplicantMessage message);
    void OnWriteMessage(WpaSupplicantMessage message);

    void OnAddressAssigned(const std::string &address);

private:
    void StartService();
    void ConnectSupplicant();
    void Request(const WpaSupplicantMessage &message, std::function<void(WpaSupplicantMessage)> callback);
    bool CheckResult(const std::string &result);

    static gboolean OnSupplicantConnected(gpointer user_data);
    static void OnSupplicantWatch(GPid pid, gint status, gpointer user_data);
    static gboolean OnGroupClientDhcpTimeout(gpointer user_data);
    static gboolean OnDeviceFailureTimeout(gpointer user_data);
    static gboolean OnIncomingMessages(GIOChannel *source, GIOCondition condition,
                                         gpointer user_data);

private:
    NetworkManager::Delegate *delegate_;
    std::string interface_name_;
    std::string ctrl_path_;
    int sock_;
    std::map<std::string,NetworkDevice::Ptr> available_devices_;
    std::unique_ptr<WpaSupplicantCommandQueue> command_queue_;
    NetworkDevice::Ptr current_peer_;
    NetworkDeviceRole current_role_;
    DhcpClient dhcp_client_;
    DhcpServer dhcp_server_;
    GPid supplicant_pid_;
    GIOChannel *channel_;
    guint dhcp_timeout_;
};

#endif