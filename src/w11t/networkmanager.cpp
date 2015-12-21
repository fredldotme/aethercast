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

#include <sys/select.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>
#include <sys/prctl.h>

#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <chrono>
#include <unordered_map>

#include <boost/filesystem.hpp>

#include <mcs/networkdevice.h>
#include <mcs/networkutils.h>
#include <mcs/utils.h>
#include <mcs/logger.h>

#include "networkmanager.h"
#include "wfddeviceinfo.h"


namespace {
constexpr const char *kWpaSupplicantBinPath{"/sbin/wpa_supplicant"};
constexpr const uint kReadBufferSize{1024};
const std::chrono::milliseconds kDhcpIpAssignmentTimeout{5000};
const std::chrono::milliseconds kPeerFailureTimeout{5000};
const uint kSupplicantRespawnLimit{10};
const std::chrono::milliseconds kSupplicantRespawnTimeout{2000};

constexpr const char *kP2pDeviceFound{"P2P-DEVICE-FOUND"};
constexpr const char *kP2pDeviceLost{"P2P-DEVICE-LOST"};
constexpr const char *kP2pGroupFormationSuccess{"P2P-GROUP-FORMATION-SUCCESS"};
constexpr const char *kP2pGroupStarted{"P2P-GROUP-STARTED"};
constexpr const char *kP2pGroupRemoved{"P2P-GROUP-REMOVED"};
constexpr const char *kP2pGoNegFailure{"P2P-GO-NEG-FAILURE"};
constexpr const char *kP2pFindStopped{"P2P-FIND-STOPPED"};
constexpr const char *kApStaConnected{"AP-STA-CONNECTED"};
constexpr const char *kApStaDisconnected{"AP-STA-DISCONNECTED"};
constexpr const char *kCtrlEventScanStarted{"CTRL-EVENT-SCAN-STARTED"};
constexpr const char *kCtrlEventScanResults{"CTRL-EVENT-SCAN-RESULTS"};
constexpr const char *kCtrlEventConnected{"CTRL-EVENT-CONNECTED"};
constexpr const char *kCtrlEventDisconnected{"CTRL-EVENT-DISCONNECTED"};
}

namespace w11t {
NetworkManager::NetworkManager() :
    delegate_(nullptr),
    // This network manager implementation is bound to the p2p0 network interface
    // being available which is the case on most Android platforms.
    interface_name_("p2p0"),
    firmware_loader_(interface_name_, this),
    ctrl_path_(mcs::Utils::Sprintf("/var/run/%s_supplicant", interface_name_.c_str())),
    command_queue_(new CommandQueue(this)),
    dhcp_client_(this, interface_name_),
    dhcp_server_(nullptr, interface_name_),
    channel_(nullptr)   ,
    channel_watch_(0),
    dhcp_timeout_(0),
    respawn_limit_(kSupplicantRespawnLimit),
    respawn_source_(0),
    is_group_owner_(false),
    scanning_(false) {
}

NetworkManager::~NetworkManager() {
    DisconnectSupplicant();
    StopSupplicant();

    if (respawn_source_)
        g_source_remove(respawn_source_);
}

void NetworkManager::SetDelegate(mcs::NetworkManager::Delegate *delegate) {
    delegate_ = delegate;
}

bool NetworkManager::Setup() {
    if (!firmware_loader_.IsNeeded())
        return StartSupplicant();

    return firmware_loader_.TryLoad();
}

void NetworkManager::OnFirmwareLoaded() {
    StartSupplicant();
}

void NetworkManager::OnFirmwareUnloaded() {
    StopSupplicant();

    // FIXME what are we going to do now? This needs to be
    // solved together with the other system components
    // changing the firmware. Trying to reload the firmware
    // is the best we can do for now.
    firmware_loader_.TryLoad();
}

void NetworkManager::OnUnsolicitedResponse(Message message) {
    if (message.ItsType() != Message::Type::kEvent) {
        MCS_WARNING("unhandled supplicant message: %s", message.Raw().c_str());
        return;
    }
    
    // Ignore events we are not interested in
    if (message.Name() == kCtrlEventScanStarted ||
        message.Name() == kCtrlEventScanResults ||
        message.Name() == kCtrlEventConnected ||
        message.Name() == kCtrlEventDisconnected)
        return;

    if (message.Name() == kP2pDeviceFound)
        OnP2pDeviceFound(message);
    else if (message.Name() == kP2pDeviceLost)
        OnP2pDeviceLost(message);
    else if (message.Name() == kP2pGroupStarted)
        OnP2pGroupStarted(message);
    else if (message.Name() == kP2pGroupRemoved)
        OnP2pGroupRemoved(message);
    else if (message.Name() == kP2pGoNegFailure)
        OnP2pGoNegFailure(message);
    else if (message.Name() == kP2pFindStopped)
        OnP2pFindStopped(message);
    else if (message.Name() == kApStaConnected)
        OnApStaConnected(message);
    else if (message.Name() == kApStaDisconnected)
        OnApStaDisconnected(message);
    else
        MCS_WARNING("unhandled supplicant event: %s", message.Raw().c_str());
}

void NetworkManager::OnP2pDeviceFound(Message &message) {
    // P2P-DEVICE-FOUND 4e:74:03:70:e2:c1 p2p_dev_addr=4e:74:03:70:e2:c1
    // pri_dev_type=8-0050F204-2 name='Aquaris M10' config_methods=0x188 dev_capab=0x5
    // group_capab=0x0 wfd_dev_info=0x00111c440032 new=1
    Named<std::string> address{"p2p_dev_addr"};
    Named<std::string> name{"name"};
    Named<std::string> config_methods_str{"config_methods"};
    Named<std::string> wfd_dev_info{"wfd_dev_info"};
    message.Read(address, name, config_methods_str, wfd_dev_info);

    MCS_DEBUG("address %s name %s config_methods %s wfd_dev_info %s", address, name, config_methods_str, wfd_dev_info);

    auto wfd_info = WfdDeviceInfo::Parse(wfd_dev_info);

    if (!wfd_info.IsSupported()) {
        MCS_DEBUG("Ignoring unsupported device %s", address);
        return;
    }

    auto roles = std::vector<mcs::NetworkDeviceRole>();
    if (wfd_info.IsSupportedSink())
        roles.push_back(mcs::kSink);
    if (wfd_info.IsSupportedSource())
        roles.push_back(mcs::kSource);

    // Check if we've that peer already in our list, if that is the
    // case we just update it.
    for (auto iter : available_devices_) {
        auto device = iter.second;

        if (device->Address() != address)
            continue;

        device->Address() = address;
        device->Name() = name;
        device->SupportedRoles() = roles;

        return;
    }

    NetworkDevice::Ptr device(new NetworkDevice(address, name, roles));

    available_devices_[address] = device;

    if (delegate_)
        delegate_->OnDeviceFound(device);
}

void NetworkManager::OnP2pDeviceLost(Message &message) {
    // P2P-DEVICE-LOST p2p_dev_addr=4e:74:03:70:e2:c1

    Named<std::string> address{"p2p_dev_address"};
    message.Read(address);

    auto iter = available_devices_.find(address);
    if (iter == available_devices_.end())
        return;

    if (delegate_)
        delegate_->OnDeviceLost(iter->second);

    available_devices_.erase(iter);
}

void NetworkManager::OnP2pGroupStarted(Message &message) {
    // P2P-GROUP-STARTED p2p0 GO ssid="DIRECT-hB" freq=2412 passphrase="HtP0qYon"
    // go_dev_addr=4e:74:03:64:95:a7
    if (!current_peer_)
        return;

    std::string role;
    message.Read(skip<std::string>()).Read(role);

    AdvanceDeviceState(current_peer_, mcs::kConfiguration);

    // If we're the GO the other side is the client and vice versa
    if (role == "GO") {
        is_group_owner_ = true;

        // As we're the owner we can now just startup the DHCP server
        // and report we're connected as there is not much more to do
        // from our side.
        dhcp_server_.Start();

        AdvanceDeviceState(current_peer_, mcs::kConnected);
    } else {
        is_group_owner_ = false;

        // We're a client of a formed group now and have to acquire
        // our IP address via DHCP so we have to wait until we're
        // reporting our upper layers that we're connected.
        dhcp_client_.Start();

        // To not wait forever we're starting a timeout here which
        // will bring everything down if we didn't received a IP
        // address once it happens.
        dhcp_timeout_ = g_timeout_add(kDhcpIpAssignmentTimeout.count(), &NetworkManager::OnGroupClientDhcpTimeout, this);
    }
}

void NetworkManager::OnP2pGroupRemoved(Message &message) {
    // P2P-GROUP-REMOVED p2p0 GO reason=FORMATION_FAILED
    if (!current_peer_)
        return;

    // FIXME this can be made easier once we have the same interface for
    // both client and server to that we only do an dhcp_.Stop() without
    // carrying if its a server or a client.
    if (is_group_owner_)
        dhcp_server_.Stop();
    else
        dhcp_client_.Stop();
    
    Named<std::string> reason{"reason"};
    message.Read(skip<std::string>(), skip<std::string>(), reason);

    static std::unordered_map<std::string, mcs::NetworkDeviceState> lut {
        {"FORMATION_FAILED", mcs::kFailure},
        {"PSK_FAILURE", mcs::kFailure},
        {"FREQ_CONFLICT", mcs::kFailure},
    };

    AdvanceDeviceState(current_peer_, lut.count(reason) > 0 ? lut.at(reason) : mcs::kDisconnected);
    current_peer_.reset();
}

void NetworkManager::OnP2pGoNegFailure(Message &message) {
    if (!current_peer_)
        return;

    AdvanceDeviceState(current_peer_, mcs::kFailure);

    current_peer_.reset();
}

void NetworkManager::OnP2pFindStopped(Message &message) {
    if (!scanning_)
        return;

    scanning_ = false;

    if (delegate_)
        delegate_->OnChanged();
}

void NetworkManager::OnApStaConnected(Message &message) {
}

void NetworkManager::OnApStaDisconnected(Message &message) {
}

void NetworkManager::OnWriteMessage(Message message) {
    auto data = message.Raw();
    if (::send(sock_, data.c_str(), data.length(), 0) < 0)
        MCS_WARNING("Failed to send data to wpa-supplicant");
}

mcs::IpV4Address NetworkManager::LocalAddress() const {
    mcs::IpV4Address address;

    if (is_group_owner_)
        address = dhcp_server_.LocalAddress();
    else
        address = dhcp_client_.LocalAddress();

    return address;
}

bool NetworkManager::Running() const {
    return supplicant_pid_ > 0;
}

bool NetworkManager::Scanning() const {
    return scanning_;
}

gboolean NetworkManager::OnConnectSupplicant(gpointer user_data) {
    auto inst = static_cast<NetworkManager*>(user_data);
    // If we're no able to connect to supplicant we try it again next time
    return !inst->ConnectSupplicant();
}

void NetworkManager::OnSupplicantWatch(GPid pid, gint status, gpointer user_data) {
    auto inst = static_cast<NetworkManager*>(user_data);

    MCS_WARNING("Supplicant process exited with status %d", status);

    if (!g_spawn_check_exit_status(status, nullptr))
        inst->HandleSupplicantFailed();
}

gboolean NetworkManager::OnSupplicantRespawn(gpointer user_data) {
    auto inst = static_cast<NetworkManager*>(user_data);

    if (!inst->StartSupplicant() && inst->respawn_limit_ > 0) {
        // If we directly failed to start supplicant we schedule the next try
        // right away
        inst->respawn_limit_--;
        return TRUE;
    }

    return FALSE;
}

void NetworkManager::HandleSupplicantFailed() {
    if (respawn_limit_ > 0) {
        if (respawn_source_)
            g_source_remove(respawn_source_);

        respawn_source_ = g_timeout_add(kSupplicantRespawnTimeout.count(), &NetworkManager::OnSupplicantRespawn, this);
        respawn_limit_--;
    }

    DisconnectSupplicant();
    StopSupplicant();
    Reset();
}

void NetworkManager::Reset() {
    if (current_peer_) {
        AdvanceDeviceState(current_peer_, mcs::kDisconnected);

        current_peer_ = nullptr;

        if (dhcp_timeout_ > 0) {
            g_source_remove(dhcp_timeout_);
            dhcp_timeout_ = 0;
        }

        dhcp_client_.Stop();
        dhcp_server_.Stop();
    }

    if (delegate_) {
        for (auto peer : available_devices_)
            delegate_->OnDeviceLost(peer.second);
    }

    available_devices_.clear();
    is_group_owner_ = false;
}

bool NetworkManager::CreateSupplicantConfig(const std::string &conf_path) {
    auto config = std::string(
                "# GENERATED - DO NOT EDIT!\n"
                "config_methods=pbc\n" // We're only supporting PBC for now
                "ap_scan=1\n");

    GError *error = nullptr;
    if (!g_file_set_contents(conf_path.c_str(), config.c_str(), config.length(), &error)) {
        MCS_ERROR("Failed to create configuration file for supplicant: %s",
                  error->message);
        g_error_free(error);
        return false;
    }

    return true;
}

void NetworkManager::OnSupplicantProcessSetup(gpointer user_data) {
    // Die when our parent dies so we don't stay around any longer and can
    // be restarted when the service restarts
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0)
        MCS_ERROR("Failed to track parents process status: %s", strerror(errno));
}

bool NetworkManager::StartSupplicant() {
    auto conf_path = mcs::Utils::Sprintf("/tmp/supplicant-%s.conf", interface_name_.c_str());

    if (!CreateSupplicantConfig(conf_path))
        return false;

    // Drop any left over control socket to be able to setup
    // a new one.
    boost::system::error_code err_code;
    auto path = boost::filesystem::path(ctrl_path_);
    boost::filesystem::remove_all(path, err_code);
    if (err_code)
        MCS_ERROR("Failed remove control directory for supplicant. Will cause problems.");

    auto cmdline = mcs::Utils::Sprintf("%s -Dnl80211 -i%s -C%s -ddd -t -K -c%s -W",
                                           kWpaSupplicantBinPath,
                                           interface_name_.c_str(),
                                           ctrl_path_.c_str(),
                                           conf_path.c_str());
    auto argv = g_strsplit(cmdline.c_str(), " ", -1);

    auto flags = G_SPAWN_DEFAULT | G_SPAWN_DO_NOT_REAP_CHILD;

    if (!getenv("MIRACAST_SUPPLICANT_DEBUG"))
        flags |= (G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL);

    GError *error = nullptr;
    int err = g_spawn_async(NULL, argv, NULL, (GSpawnFlags) flags,
                            &NetworkManager::OnSupplicantProcessSetup, NULL, &supplicant_pid_, &error);
    if (err < 0) {
        MCS_ERROR("Failed to spawn wpa-supplicant process: %s", error->message);
        g_strfreev(argv);
        g_error_free(error);
        return false;
    }

    err = g_child_watch_add(supplicant_pid_, &NetworkManager::OnSupplicantWatch, this);
    if (err < 0) {
        MCS_ERROR("Failed to setup watch for supplicant");
        StopSupplicant();
        return false;
    }

    g_strfreev(argv);

    g_timeout_add(500, &NetworkManager::OnConnectSupplicant, this);

    return true;
}

void NetworkManager::StopSupplicant() {
    if (supplicant_pid_ < 0)
        return;

    g_spawn_close_pid(supplicant_pid_);
    supplicant_pid_ = 0;
}

bool NetworkManager::ConnectSupplicant() {
    std::string socket_path = mcs::Utils::Sprintf("%s/%s",
                                                      ctrl_path_.c_str(),
                                                      interface_name_.c_str());


    MCS_DEBUG("Connecting supplicant on %s", socket_path.c_str());

    struct sockaddr_un local;
    sock_ = ::socket(PF_UNIX, SOCK_DGRAM, 0);
    if (sock_ < 0) {
        MCS_ERROR("Failed to create socket");
        return false;
    }

    local.sun_family = AF_UNIX;

    std::string local_path = mcs::Utils::Sprintf("/tmp/p2p0-%d", getpid());
    if (g_file_test(local_path.c_str(), G_FILE_TEST_EXISTS))
        g_remove(local_path.c_str());

    strncpy(local.sun_path, local_path.c_str(), sizeof(local.sun_path));

    if (::bind(sock_, (struct sockaddr *) &local, sizeof(local)) < 0) {
        MCS_ERROR("Failed to bind socket");
        return false;
    }

    struct sockaddr_un dest;
    dest.sun_family = AF_UNIX;
    strncpy(dest.sun_path, socket_path.c_str(), sizeof(dest.sun_path));

    if (::connect(sock_, (struct sockaddr*) &dest, sizeof(dest)) < 0) {
        MCS_ERROR("Failed to connect socket");
        return false;
    }

    int flags = ::fcntl(sock_, F_GETFL);
    flags |= O_NONBLOCK;
    ::fcntl(sock_, F_SETFL, flags);

    channel_ = g_io_channel_unix_new(sock_);
    channel_watch_ = g_io_add_watch(channel_, (GIOCondition) (G_IO_IN | G_IO_HUP | G_IO_ERR),
                       &NetworkManager::OnIncomingMessages, this);
    if (channel_watch_ == 0) {
        MCS_ERROR("Failed to setup watch for incoming messages from wpa-supplicant");
        return false;
    }

    // We need to attach to receive all occuring events from wpa-supplicant
    auto m = Message::CreateRequest("ATTACH");
    RequestAsync(m, [=](const Message &message) {
        if (message.IsFail()) {
            MCS_ERROR("Failed to attach to wpa-supplicant for unsolicited events");
            return;
        }
    });

    // Enable WiFi display support
    RequestAsync(Message::CreateRequest("SET") << "wifi_display" << std::int32_t{1});

    std::list<std::string> wfd_sub_elements;
    // FIXME build this rather than specifying a static string here
    wfd_sub_elements.push_back(std::string("000600101C440032"));
    SetWfdSubElements(wfd_sub_elements);

    respawn_limit_ = kSupplicantRespawnLimit;

    return true;
}

void NetworkManager::DisconnectSupplicant() {
    if (sock_ < 0)
        return;

    if (channel_) {
        g_io_channel_shutdown(channel_, FALSE, nullptr);
        g_io_channel_unref(channel_);
        channel_ = nullptr;
    }

    if (channel_watch_ > 0) {
        g_source_remove(channel_watch_);
        channel_watch_ = 0;
    }

    if (sock_ > 0) {
        ::close(sock_);
        sock_ = 0;
    }
}

gboolean NetworkManager::OnIncomingMessages(GIOChannel *source, GIOCondition condition,
                                                       gpointer user_data) {
    auto inst = static_cast<NetworkManager*>(user_data);
    char buf[kReadBufferSize];

    if (condition & G_IO_HUP) {
        inst->StopSupplicant();
        return TRUE;
    }

    while (mcs::NetworkUtils::BytesAvailableToRead(inst->sock_) > 0) {
        int ret = recv(inst->sock_, buf, sizeof(buf) - 1, 0);
        if (ret < 0)
            return TRUE;

        buf[ret] = '\0';

        inst->command_queue_->HandleMessage(Message::Parse(buf));
    }

    return TRUE;
}

void NetworkManager::RequestAsync(const Message &message, std::function<void(Message)> callback) {
    command_queue_->EnqueueCommand(message, callback);
}

void NetworkManager::OnAddressAssigned(const mcs::IpV4Address &address) {
    if (!current_peer_)
        return;

    if (dhcp_timeout_ > 0) {
        g_source_remove(dhcp_timeout_);
        dhcp_timeout_ = 0;
    }


    AdvanceDeviceState(current_peer_, mcs::kConnected);
}

gboolean NetworkManager::OnDeviceFailureTimeout(gpointer user_data) {
    auto inst = static_cast<NetworkManager*>(user_data);
    inst->current_peer_->State() = mcs::kIdle;

    return FALSE;
}

gboolean NetworkManager::OnGroupClientDhcpTimeout(gpointer user_data) {
    auto inst = static_cast<NetworkManager*>(user_data);

    if (!inst->current_peer_)
        return FALSE;

    // Switch peer back into idle state after some time
    g_timeout_add(kPeerFailureTimeout.count(), &NetworkManager::OnDeviceFailureTimeout, inst);

    inst->AdvanceDeviceState(inst->current_peer_, mcs::kFailure);

    return FALSE;
}

void NetworkManager::SetWfdSubElements(const std::list<std::string> &elements) {
    int n = 0;
    for (auto element : elements) {
        auto m = Message::CreateRequest("WFD_SUBELEM_SET") << n << element;
        RequestAsync(Message::CreateRequest("WFD_SUBELEM_SET") << n << element);
        n++;
    }
}

void NetworkManager::Scan(const std::chrono::seconds &timeout) {
    if (scanning_)
        return;

    // This will scan forever but is exactly what we want as our user
    // has to take care about stopping this scan after some time.
    auto m = Message::CreateRequest("P2P_FIND");

    if (timeout.count() > 0) {
        m << timeout.count();
    }

    RequestAsync(m, [=](const Message &message) {
        auto scanning = !message.IsFail();

        if (scanning == scanning_)
            return;

        scanning_ = scanning;

        if (delegate_)
            delegate_->OnChanged();
    });
}

std::vector<mcs::NetworkDevice::Ptr> NetworkManager::Devices() const {
    std::vector<mcs::NetworkDevice::Ptr> values;
    std::transform(available_devices_.begin(), available_devices_.end(),
                   std::back_inserter(values),
                   [=](const std::pair<std::string,mcs::NetworkDevice::Ptr> &value) {
        return value.second;
    });
    return values;
}

void NetworkManager::AdvanceDeviceState(const NetworkDevice::Ptr &device, mcs::NetworkDeviceState state) {
    MCS_DEBUG("new state %s", mcs::NetworkDevice::StateToStr(state).c_str());

    device->State() = state;
    if (delegate_) {
        delegate_->OnDeviceStateChanged(device);
        delegate_->OnDeviceChanged(device);
    }
}

bool NetworkManager::Connect(const mcs::NetworkDevice::Ptr &device) {
    if (current_peer_)
        return false;

    if (available_devices_.find(device->Address()) == available_devices_.end())
        return false;

    current_peer_ = available_devices_[device->Address()];

    MCS_DEBUG("Attempting to connect with %s", device->Address());

    if (scanning_) {
        MCS_DEBUG("Currently scanning; stopping this first");
        RequestAsync(Message::CreateRequest("P2P_STOP_FIND"));
    }

    MCS_DEBUG("Now sending connect request to wpa");
    auto m = Message::CreateRequest("P2P_CONNECT") << device->Address() << "pbc";
    
    RequestAsync(m, [&](const Message &message) {
        if (message.IsFail()) {
            AdvanceDeviceState(current_peer_, mcs::kFailure);
            MCS_ERROR("Failed to connect with remote %s", device->Address().c_str());
            return;
        }
    });

    return true;
}

bool NetworkManager::Disconnect(const mcs::NetworkDevice::Ptr &device) {
    if (!current_peer_ || current_peer_ != device)
        return false;

    MCS_DEBUG("device %s", device->Address());

    RequestAsync(
        current_peer_->State() == mcs::kAssociation ?
            Message::CreateRequest("P2P_CANCEL") :
            Message::CreateRequest("P2P_GROUP_REMOVE") << interface_name_,
        [&](const Message &message) {
            if (message.IsFail()) {
                MCS_ERROR("Failed to disconnect all connected devices on interface %s", interface_name_.c_str());
                return;
            }
        });

    return true;
}
}