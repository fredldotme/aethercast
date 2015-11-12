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

#include <QDebug>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusObjectPath>
#include <QTimer>
#include <QFile>

#include "miracastservice.h"
#include "networkp2pmanagerwpasupplicant.h"
#include "wfddeviceinfo.h"

#define MIRACAST_DEFAULT_RTSP_CTRL_PORT     7236

MiracastService::MiracastService() :
    currentState(NetworkP2pDevice::Idle)
{
    if (!QFile::exists("/sys/class/net/p2p0/uevent"))
        loadRequiredFirmware();

    manager = new NetworkP2pManagerWpaSupplicant(this, "p2p0");

    QTimer::singleShot(200, [=]() {
        manager->setup();
    });

    connect(&source, SIGNAL(clientDisconnected()),
            this, SLOT(onSourceClientDisconnected()));
}

MiracastService::~MiracastService()
{
}

NetworkP2pDevice::State MiracastService::state() const
{
    return currentState;
}

void MiracastService::loadRequiredFirmware()
{
    qDebug() << "Switching device WiFi chip firmware to get P2P support";

    QDBusInterface iface("fi.w1.wpa_supplicant1", "/fi/w1/wpa_supplicant1",
                         "fi.w1.wpa_supplicant1", QDBusConnection::systemBus());

    if (!iface.isValid()) {
        qWarning() << "Could not reach wpa-supplicant on dbus";
        return;
    }

    QDBusReply<void> reply = iface.call("SetInterfaceFirmware",
                                QVariant::fromValue(QDBusObjectPath("/fi/w1/wpa_supplicant1/Interfaces/1")),
                                "p2p");
    if (!reply.isValid()) {
        qDebug() << "Failed to switch WiFi chip firmware:" << reply.error();
        return;
    }
}

void MiracastService::advanceState(NetworkP2pDevice::State newState)
{
    QString address;

    switch (newState) {
    case NetworkP2pDevice::Association:
        break;

    case NetworkP2pDevice::Connected:
        // We've have to pick the right address we need to tell our source to
        // push all streaming data to.
        address = currentPeer->address();
        if (manager->role() == NetworkP2pDevice::GroupOwner)
            address = manager->localAddress();

        source.setup(address, MIRACAST_DEFAULT_RTSP_CTRL_PORT);

        finishConnectAttempt(true);

        break;

    case NetworkP2pDevice::Failure:
        if (currentState == NetworkP2pDevice::Association)
            finishConnectAttempt(false, "Failed to connect remote device");

    case NetworkP2pDevice::Disconnected:
        if (currentState == NetworkP2pDevice::Connected)
            source.release();

        startIdleTimer();
        break;

    case NetworkP2pDevice::Idle:
        break;

    default:
        break;
    }


    currentState = newState;
    Q_EMIT stateChanged();
}

void MiracastService::peerConnected(const NetworkP2pDevice::Ptr &peer)
{
    advanceState(NetworkP2pDevice::Connected);
}

void MiracastService::peerDisconnected(const NetworkP2pDevice::Ptr &peer)
{
    advanceState(NetworkP2pDevice::Disconnected);

    currentPeer.clear();
}

void MiracastService::peerFailed(const NetworkP2pDevice::Ptr &peer)
{
    advanceState(NetworkP2pDevice::Failure);

    currentPeer.clear();

    finishConnectAttempt(false, "Failed to connect device");
}

void MiracastService::peerChanged(const NetworkP2pDevice::Ptr &peer)
{
}

void MiracastService::onSourceClientDisconnected()
{
    QTimer::singleShot(0, [=] {
        advanceState(NetworkP2pDevice::Failure);
        currentPeer.clear();
    });
}

void MiracastService::startIdleTimer()
{
    QTimer::singleShot(1000, [&]() {
        advanceState(NetworkP2pDevice::Idle);
    });
}

void MiracastService::finishConnectAttempt(bool success, const QString &errorText)
{
    if (connectCallback)
        connectCallback(success, errorText);

    connectCallback = nullptr;
}

void MiracastService::connectSink(const QString &address, std::function<void(bool,QString)> callback)
{
    if (!currentPeer.isNull()) {
        callback(false, "Already connected");
        return;
    }

    NetworkP2pDevice::Ptr device;

    for (auto peer : manager->peers()) {
        if (peer->address() != address)
            continue;

        device = peer;
        break;
    }

    if (device.isNull()) {
        callback(false, "Couldn't find device");
        return;
    }

    if (manager->connect(device->address(), false) < 0) {
        callback(false, "Failed to connect with remote device");
        return;
    }

    currentPeer = device;
    connectCallback = callback;
}
