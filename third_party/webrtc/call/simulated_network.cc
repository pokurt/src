/*
 *  Copyright 2018 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "call/simulated_network.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "rtc_base/checks.h"

namespace webrtc {

SimulatedNetwork::SimulatedNetwork(SimulatedNetwork::Config config,
                                   uint64_t random_seed)
    : random_(random_seed), bursting_(false) {
  SetConfig(config);
}

SimulatedNetwork::~SimulatedNetwork() = default;

void SimulatedNetwork::SetConfig(const SimulatedNetwork::Config& config) {
  rtc::CritScope crit(&config_lock_);
  config_state_.config = config;  // Shallow copy of the struct.
  double prob_loss = config.loss_percent / 100.0;
  if (config_state_.config.avg_burst_loss_length == -1) {
    // Uniform loss
    config_state_.prob_loss_bursting = prob_loss;
    config_state_.prob_start_bursting = prob_loss;
  } else {
    // Lose packets according to a gilbert-elliot model.
    int avg_burst_loss_length = config.avg_burst_loss_length;
    int min_avg_burst_loss_length = std::ceil(prob_loss / (1 - prob_loss));

    RTC_CHECK_GT(avg_burst_loss_length, min_avg_burst_loss_length)
        << "For a total packet loss of " << config.loss_percent << "%% then"
        << " avg_burst_loss_length must be " << min_avg_burst_loss_length + 1
        << " or higher.";

    config_state_.prob_loss_bursting = (1.0 - 1.0 / avg_burst_loss_length);
    config_state_.prob_start_bursting =
        prob_loss / (1 - prob_loss) / avg_burst_loss_length;
  }
}

void SimulatedNetwork::PauseTransmissionUntil(int64_t until_us) {
  rtc::CritScope crit(&config_lock_);
  config_state_.pause_transmission_until_us = until_us;
}

bool SimulatedNetwork::EnqueuePacket(PacketInFlightInfo packet) {
  RTC_DCHECK_RUNS_SERIALIZED(&process_checker_);
  ConfigState state = GetConfigState();

  UpdateCapacityQueue(state, packet.send_time_us);

  packet.size += state.config.packet_overhead;

  if (state.config.queue_length_packets > 0 &&
      capacity_link_.size() >= state.config.queue_length_packets) {
    // Too many packet on the link, drop this one.
    return false;
  }

  // Set arrival time = send time for now; actual arrival time will be
  // calculated in UpdateCapacityQueue.
  queue_size_bytes_ += packet.size;
  capacity_link_.push({packet, packet.send_time_us});

  return true;
}

absl::optional<int64_t> SimulatedNetwork::NextDeliveryTimeUs() const {
  RTC_DCHECK_RUNS_SERIALIZED(&process_checker_);
  if (!delay_link_.empty())
    return delay_link_.begin()->arrival_time_us;
  return absl::nullopt;
}

void SimulatedNetwork::UpdateCapacityQueue(ConfigState state,
                                           int64_t time_now_us) {
  bool needs_sort = false;

  // Catch for thread races.
  if (time_now_us < last_capacity_link_visit_us_.value_or(time_now_us))
    return;

  int64_t time_us = last_capacity_link_visit_us_.value_or(time_now_us);
  // Check the capacity link first.
  while (!capacity_link_.empty()) {
    int64_t time_until_front_exits_us = 0;
    if (state.config.link_capacity_kbps > 0) {
      int64_t remaining_bits =
          capacity_link_.front().packet.size * 8 - pending_drain_bits_;
      RTC_DCHECK(remaining_bits > 0);
      // Division rounded up - packet not delivered until its last bit is.
      time_until_front_exits_us =
          (1000 * remaining_bits + state.config.link_capacity_kbps - 1) /
          state.config.link_capacity_kbps;
    }

    if (time_us + time_until_front_exits_us > time_now_us) {
      // Packet at front will not exit yet. Will not enter here on infinite
      // capacity(=0) so no special handling needed.
      pending_drain_bits_ +=
          ((time_now_us - time_us) * state.config.link_capacity_kbps) / 1000;
      break;
    }
    if (state.config.link_capacity_kbps > 0) {
      pending_drain_bits_ +=
          (time_until_front_exits_us * state.config.link_capacity_kbps) / 1000;
    } else {
      // Enough to drain the whole queue.
      pending_drain_bits_ = queue_size_bytes_ * 8;
    }

    // Time to get this packet.
    PacketInfo packet = capacity_link_.front();
    capacity_link_.pop();

    time_us += time_until_front_exits_us;
    RTC_DCHECK(time_us >= packet.packet.send_time_us);
    packet.arrival_time_us =
        std::max(state.pause_transmission_until_us, time_us);
    queue_size_bytes_ -= packet.packet.size;
    pending_drain_bits_ -= packet.packet.size * 8;
    RTC_DCHECK(pending_drain_bits_ >= 0);

    // Drop packets at an average rate of |state.config.loss_percent| with
    // and average loss burst length of |state.config.avg_burst_loss_length|.
    if ((bursting_ && random_.Rand<double>() < state.prob_loss_bursting) ||
        (!bursting_ && random_.Rand<double>() < state.prob_start_bursting)) {
      bursting_ = true;
      packet.arrival_time_us = PacketDeliveryInfo::kNotReceived;
    } else {
      bursting_ = false;
      int64_t arrival_time_jitter_us = std::max(
          random_.Gaussian(state.config.queue_delay_ms * 1000,
                           state.config.delay_standard_deviation_ms * 1000),
          0.0);

      // If reordering is not allowed then adjust arrival_time_jitter
      // to make sure all packets are sent in order.
      int64_t last_arrival_time_us =
          delay_link_.empty() ? -1 : delay_link_.back().arrival_time_us;
      if (!state.config.allow_reordering && !delay_link_.empty() &&
          packet.arrival_time_us + arrival_time_jitter_us <
              last_arrival_time_us) {
        arrival_time_jitter_us = last_arrival_time_us - packet.arrival_time_us;
      }
      packet.arrival_time_us += arrival_time_jitter_us;
      if (packet.arrival_time_us >= last_arrival_time_us) {
        last_arrival_time_us = packet.arrival_time_us;
      } else {
        needs_sort = true;
      }
    }
    delay_link_.emplace_back(packet);
  }
  last_capacity_link_visit_us_ = time_now_us;
  // Cannot save unused capacity for later.
  pending_drain_bits_ = std::min(pending_drain_bits_, queue_size_bytes_ * 8);

  if (needs_sort) {
    // Packet(s) arrived out of order, make sure list is sorted.
    std::sort(delay_link_.begin(), delay_link_.end(),
              [](const PacketInfo& p1, const PacketInfo& p2) {
                return p1.arrival_time_us < p2.arrival_time_us;
              });
  }
}

SimulatedNetwork::ConfigState SimulatedNetwork::GetConfigState() const {
  rtc::CritScope crit(&config_lock_);
  return config_state_;
}

std::vector<PacketDeliveryInfo> SimulatedNetwork::DequeueDeliverablePackets(
    int64_t receive_time_us) {
  RTC_DCHECK_RUNS_SERIALIZED(&process_checker_);
  UpdateCapacityQueue(GetConfigState(), receive_time_us);
  std::vector<PacketDeliveryInfo> packets_to_deliver;
  // Check the extra delay queue.
  while (!delay_link_.empty() &&
         receive_time_us >= delay_link_.front().arrival_time_us) {
    PacketInfo packet_info = delay_link_.front();
    packets_to_deliver.emplace_back(
        PacketDeliveryInfo(packet_info.packet, packet_info.arrival_time_us));
    delay_link_.pop_front();
  }
  return packets_to_deliver;
}

}  // namespace webrtc
