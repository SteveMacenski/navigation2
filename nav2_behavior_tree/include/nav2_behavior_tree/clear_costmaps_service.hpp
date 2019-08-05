// Copyright (c) 2019 Samsung Research America
// Author: Steve Macenski
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NAV2_BEHAVIOR_TREE__CLEAR_COSTMAPS_SERVICE_HPP_
#define NAV2_BEHAVIOR_TREE__CLEAR_COSTMAPS_SERVICE_HPP_

#include <string>
#include <memory>
#include <cmath>

#include "nav2_behavior_tree/bt_service_node.hpp"
#include "nav2_msgs/srv/clear_costmap_except_region.hpp"
#include "nav2_msgs/srv/clear_costmap_around_robot.hpp"
#include "nav2_msgs/srv/clear_entire_costmap.hpp"

namespace nav2_behavior_tree
{

class ClearCostmapsService : public BtServiceNode<nav2_msgs::srv::ClearEntireCostmap>
{
public:
  explicit ClearCostmapsService(const std::string & service_name)
  : BtServiceNode<nav2_msgs::srv::ClearEntireCostmap>(service_name)
  {
  }

  void on_init() override
  {
  }
};

}  // namespace nav2_behavior_tree

#endif  // NAV2_BEHAVIOR_TREE__CLEAR_COSTMAPS_SERVICE_HPP_