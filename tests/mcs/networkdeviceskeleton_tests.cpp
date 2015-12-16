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

#include <gmock/gmock.h>

#include <mcs/networkdeviceskeleton.h>

#include "mock_network_device.h"

TEST(NetworkDeviceSkeleton, ThrowsForNullptrOnConstruction) {
    EXPECT_THROW(mcs::NetworkDeviceSkeleton::Create(mcs::SharedGObject<GDBusConnection>(), "/", mcs::NetworkDevice::Ptr{}, mcs::MiracastController::Ptr{}), std::logic_error);
}

TEST(NetworkDeviceSkeleton, ForwardsCallsToImpl) {
    using namespace testing;

    const mcs::MacAddress address{"lalelu"};
    const mcs::IpV4Address ipV4Address = mcs::IpV4Address::from_string("127.0.0.1");
    const std::string name{"MeMyselfAndI"};
    const mcs::NetworkDeviceState state{mcs::NetworkDeviceState::kDisconnected};
    const std::vector<mcs::NetworkDeviceRole> roles{mcs::NetworkDeviceRole::kSource};

    auto impl = std::make_shared<MockNetworkDevice>();

    EXPECT_CALL(*impl, Address()).Times(AtLeast(1)).WillRepeatedly(Return(address));
    EXPECT_CALL(*impl, IPv4Address()).Times(AtLeast(1)).WillRepeatedly(Return(ipV4Address));
    EXPECT_CALL(*impl, Name()).Times(AtLeast(1)).WillRepeatedly(Return(name));
    EXPECT_CALL(*impl, State()).Times(AtLeast(1)).WillRepeatedly(Return(state));
    EXPECT_CALL(*impl, SupportedRoles()).Times(AtLeast(1)).WillRepeatedly(Return(roles));

    auto nds = mcs::NetworkDeviceSkeleton::Create(mcs::SharedGObject<GDBusConnection>(), "/", impl, mcs::MiracastController::Ptr{});

    EXPECT_EQ(address, nds->Address());
    EXPECT_EQ(ipV4Address, nds->IPv4Address());
    EXPECT_EQ(name, nds->Name());
    EXPECT_EQ(state, nds->State());
    EXPECT_EQ(roles, nds->SupportedRoles());}
