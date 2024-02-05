// Copyright 2023 Sony Group Corporation.
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

#include <memory>

#include "rosbag2_transport/player_service_client.hpp"

#include "rosbag2_cpp/service_utils.hpp"

#include "rosidl_typesupport_introspection_cpp/identifier.hpp"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"

#include "logging.hpp"

namespace rosbag2_transport
{

PlayerServiceClient::PlayerServiceClient(
  std::shared_ptr<rclcpp::GenericClient> cli,
  std::string service_name,
  const std::string & service_event_type,
  const rclcpp::Logger logger,
  std::shared_ptr<PlayerServiceClientManager> player_service_client_manager)
: client_(std::move(cli)),
  service_name_(std::move(service_name)),
  logger_(logger),
  player_service_client_manager_(player_service_client_manager)
{
  ts_lib_ = rclcpp::get_typesupport_library(
    service_event_type, "rosidl_typesupport_cpp");

  service_event_type_ts_ = rclcpp::get_message_typesupport_handle(
    service_event_type,
    "rosidl_typesupport_cpp",
    *ts_lib_);

  auto service_event_ts_introspection = get_message_typesupport_handle(
    service_event_type_ts_,
    rosidl_typesupport_introspection_cpp::typesupport_identifier);

  message_members_ =
    reinterpret_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(
    service_event_ts_introspection->data);
}

bool PlayerServiceClient::is_include_request_message(
  const rclcpp::SerializedMessage & message)
{
  auto [type, client_id, sequence_number] = get_msg_event_type(message);

  // Ignore response message
  if (type == service_msgs::msg::ServiceEventInfo::RESPONSE_SENT ||
    type == service_msgs::msg::ServiceEventInfo::RESPONSE_RECEIVED)
  {
    return false;
  }

  // For each Client, decide which request data to use based on the first message related to
  // the request that is obtained from the record data.
  // e.g.

  auto iter = request_info.find(client_id);
  if (type == service_msgs::msg::ServiceEventInfo::REQUEST_RECEIVED) {
    if (!service_set_introspection_content) {
      if (rosbag2_cpp::introspection_include_metadata_and_contents(message.size())) {
        service_set_introspection_content = true;
      }
    }

    if (iter != request_info.end()) {
      switch (iter->second) {
        case request_info_from::CLIENT:
          {
            // Already decide using request data from client.
            return false;
          }
        case request_info_from::NO_CONTENT:
          {
            if (service_set_introspection_content) {
              // introspection type is changed from metadata to metadata + contents
              request_info[client_id] = request_info_from::SERVICE;
              return true;
            } else {
              RCUTILS_LOG_WARN_ONCE_NAMED(
                ROSBAG2_TRANSPORT_PACKAGE_NAME,
                "The configuration of introspection for '%s' is metadata !",
                service_name_.c_str());
              return false;
            }
          }
        default:  // request_info_from::SERVICE:
          {
            // Already decide using request data from service.
            return true;
          }
      }
    } else {
      if (service_set_introspection_content) {
        request_info[client_id] = request_info_from::SERVICE;
        return true;
      } else {
        request_info[client_id] = request_info_from::NO_CONTENT;  // Only have metadata
        return false;
      }
    }
  }

  // type is service_msgs::msg::ServiceEventInfo::REQUEST_SENT
  if (iter != request_info.end()) {
    switch (iter->second) {
      case request_info_from::CLIENT:
        {
          // Already decide using request data from client.
          return true;
        }
      case request_info_from::NO_CONTENT:
        {
          if (rosbag2_cpp::introspection_include_metadata_and_contents(message.size())) {
            // introspection type is changed from metadata to metadata + contents
            request_info[client_id] = request_info_from::CLIENT;
            return true;
          } else {
            RCUTILS_LOG_WARN_NAMED(
              ROSBAG2_TRANSPORT_PACKAGE_NAME,
              "The configuration of introspection for '%s' client [ID: %s]` is metadata !",
              rosbag2_cpp::client_id_to_string(client_id).c_str(),
              service_name_.c_str());
            return false;
          }
        }
      default:  // request_info_from::SERVICE:
        {
          // Already decide using request data from service.
          return false;
        }
    }
  } else {
    if (rosbag2_cpp::introspection_include_metadata_and_contents(message.size())) {
      request_info[client_id] = request_info_from::CLIENT;
    } else {
      request_info[client_id] = request_info_from::NO_CONTENT;
    }
  }

  return true;
}

void PlayerServiceClient::async_send_request(const rclcpp::SerializedMessage & message)
{
  int ret = RMW_RET_OK;

  {
    auto ros_message = std::make_unique<uint8_t[]>(message_members_->size_of_);

    message_members_->init_function(
      ros_message.get(), rosidl_runtime_cpp::MessageInitialization::ZERO);

    ret = rmw_deserialize(
      &message.get_rcl_serialized_message(), service_event_type_ts_, ros_message.get());
    if (ret == RMW_RET_OK) {
      if (client_->service_is_ready()) {
        // members_[0]: info, members_[1]: request, members_[2]: response
        auto request_offset = message_members_->members_[1].offset_;
        auto request_addr = reinterpret_cast<size_t>(ros_message.get()) + request_offset;
        auto future_and_request_id = client_->async_send_request(
          reinterpret_cast<void *>(*reinterpret_cast<size_t *>(request_addr)));
        player_service_client_manager_->register_request_future(future_and_request_id);
      } else {
        RCLCPP_ERROR(
          logger_, "Service request hasn't been sent. The '%s' service isn't ready !",
          service_name_.c_str());
      }
    }

    message_members_->fini_function(ros_message.get());
  }

  if (ret != RMW_RET_OK) {
    throw std::runtime_error(
            "Failed to deserialize service event message for " + service_name_ + " !");
  }
}

std::tuple<uint8_t, PlayerServiceClient::client_id, int64_t>
PlayerServiceClient::get_msg_event_type(
  const rclcpp::SerializedMessage & message)
{
  auto msg = service_msgs::msg::ServiceEventInfo();

  const rosidl_message_type_support_t * type_support_info =
    rosidl_typesupport_cpp::
    get_message_type_support_handle<service_msgs::msg::ServiceEventInfo>();
  if (type_support_info == nullptr) {
    throw std::runtime_error(
            "Failed to get message type support handle of service event info !");
  }

  auto ret = rmw_deserialize(
    &message.get_rcl_serialized_message(),
    type_support_info,
    reinterpret_cast<void *>(&msg));
  if (ret != RMW_RET_OK) {
    throw std::runtime_error("Failed to deserialize message !");
  }

  return {msg.event_type, msg.client_gid, msg.sequence_number};
}

PlayerServiceClientManager::PlayerServiceClientManager(
  std::chrono::seconds requst_future_timeout,
  size_t maximum_request_future_queue)
: request_future_timeout_(std::chrono::seconds(requst_future_timeout)),
  maximum_request_future_queue_(maximum_request_future_queue)
{
}

bool PlayerServiceClientManager::request_future_queue_is_full()
{
  std::lock_guard<std::mutex> lock(request_futures_list_lock_);

  // To improve performance, it's not necessary to clean up completed requests and timeout requests
  // every time.
  if (request_futures_list_.size() < maximum_request_future_queue_) {
    return false;
  }

  // Remove complete request future
  remove_complete_request_future();

  // Remove all timeout request future
  remove_all_timeout_request_future();

  if (request_futures_list_.size() == maximum_request_future_queue_) {
    return true;
  }

  return false;
}

bool PlayerServiceClientManager::register_request_future(
  rclcpp::GenericClient::FutureAndRequestId & request_future)
{
  auto future_and_request_id =
    std::make_unique<rclcpp::GenericClient::FutureAndRequestId>(std::move(request_future));

  if (!request_future_queue_is_full()) {
    std::lock_guard<std::mutex> lock(request_futures_list_lock_);
    request_futures_list_[std::chrono::steady_clock::now()] = std::move(future_and_request_id);
    return true;
  } else {
    ROSBAG2_TRANSPORT_LOG_WARN(
      "Client request queue is full. "
      "Please consider increasing the length of the queue.");
  }

  return false;
}

void PlayerServiceClientManager::remove_complete_request_future()
{
  std::vector<time_point> remove_keys;
  for (auto & request_future : request_futures_list_) {
    if (request_future.second->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      remove_keys.emplace_back(request_future.first);
    }
  }
  for (auto & key : remove_keys) {
    request_futures_list_.erase(key);
  }
}

void PlayerServiceClientManager::remove_all_timeout_request_future()
{
  auto current_time = std::chrono::steady_clock::now();
  auto first_iter_without_timeout =
    request_futures_list_.lower_bound(current_time - request_future_timeout_);

  if (first_iter_without_timeout == request_futures_list_.begin()) {
    return;
  }

  auto last_iter_with_timeout = --first_iter_without_timeout;
  request_futures_list_.erase(request_futures_list_.begin(), last_iter_with_timeout);
  ROSBAG2_TRANSPORT_LOG_WARN(
    "Client requests are discarded since timeout. "
    "Please consider setting a longer timeout.");
}
}  // namespace rosbag2_transport
