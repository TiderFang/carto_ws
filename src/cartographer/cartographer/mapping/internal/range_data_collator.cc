/*
 * Copyright 2018 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cartographer/mapping/internal/range_data_collator.h"

#include <memory>

#include "absl/memory/memory.h"
#include "cartographer/mapping/local_slam_result_data.h"
#include "glog/logging.h"

namespace cartographer {
namespace mapping {

///����ô������������ݴ��� ��õ��ƽṹ��ʱ����Ϊ��ǰ����ʱ�� ȡ���������������еĵ�
///ʱ����ڵ�ǰ��ʼʱ�䵽����ʱ���ĵ��� ���޳����д����������еĵ� ʱ����ڽ���ʱ��֮ǰ��
///����ô������������ݲ����� ��ϣ���ô������������ݴ��� ȡ��������������������ʱ����Ϊ��ǰ����ʱ��
///����: ������id ��ʱ��ĵ�������
///���: ��������ʱ����ڵ�ǰ��ʼ��ֹʱ����ڵĵ��Ƶĵ�
sensor::TimedPointCloudOriginData RangeDataCollator::AddRangeData(
    const std::string& sensor_id,
    const sensor::TimedPointCloudData& timed_point_cloud_data) {
  CHECK_NE(expected_sensor_ids_.count(sensor_id), 0);
  // TODO(gaschler): These two cases can probably be one.
  ///ʹ��count�����ص��Ǳ�����Ԫ�صĸ���������У�����1�����򣬷���0��
  ///�ô���������
  if (id_to_pending_data_.count(sensor_id) != 0) {
    current_start_ = current_end_;
    // When we have two messages of the same sensor, move forward the older of
    // the two (do not send out current).
    current_end_ = id_to_pending_data_.at(sensor_id).time; ///�ô�������������ʱ��
    /// ȡ���������������еĵ� ʱ����ڵ�ǰ��ʼʱ�䵽����ʱ���ĵ���
    /// ���޳����д����������еĵ� ʱ����ڽ���ʱ��֮ǰ��
    auto result = CropAndMerge();
    id_to_pending_data_.emplace(sensor_id, timed_point_cloud_data); ///�����������
    return result;
  }
  ///��������ڸô����� ���
  id_to_pending_data_.emplace(sensor_id, timed_point_cloud_data);
  ///�������ô���������
  if (expected_sensor_ids_.size() != id_to_pending_data_.size()) {
    return {};
  }
  current_start_ = current_end_;
  // We have messages from all sensors, move forward to oldest.
  common::Time oldest_timestamp = common::Time::max(); ///���д���������ʱ�������ϵ�ʱ��
  for (const auto& pair : id_to_pending_data_) {
    oldest_timestamp = std::min(oldest_timestamp, pair.second.time);
  }
  current_end_ = oldest_timestamp;
  return CropAndMerge();
}

/// ȡ���������������еĵ� ʱ����ڵ�ǰ��ʼʱ�䵽����ʱ���ĵ���
/// ���޳����д����������еĵ� ʱ����ڽ���ʱ��֮ǰ��
sensor::TimedPointCloudOriginData RangeDataCollator::CropAndMerge() {
    ///���д����������еĵ� ʱ���ڵ�ǰ��ʼʱ�䵽��ֹʱ���е�
  sensor::TimedPointCloudOriginData result{current_end_, {}, {}};
  bool warned_for_dropped_points = false;
  ///ÿһ������������
  for (auto it = id_to_pending_data_.begin();
       it != id_to_pending_data_.end();) {
    sensor::TimedPointCloudData& data = it->second;
    sensor::TimedPointCloud& ranges = it->second.ranges;

    ///�ҵ� ��ʼʱ��<�����еĵ�ʱ��+����ʱ��<=����ʱ�� �����е�
    auto overlap_begin = ranges.begin(); ///ʱ�䷶Χ�ڵ���ʼ
    ///����ʱ��+��ʱ��<��ǰ��ʼʱ������е�
    while (overlap_begin < ranges.end() &&
           data.time + common::FromSeconds((*overlap_begin).time) <
               current_start_) {
      ++overlap_begin;
    }
    ///����ʱ��+��ʱ��<=��ǰ����ʱ������е�
    auto overlap_end = overlap_begin;
    while (overlap_end < ranges.end() &&
           data.time + common::FromSeconds((*overlap_end).time) <=
               current_end_) {
      ++overlap_end;
    }
    ///��������vector��ʱ�����ʼʱ����ĵ���
    if (ranges.begin() < overlap_begin && !warned_for_dropped_points) {
      LOG(WARNING) << "Dropped " << std::distance(ranges.begin(), overlap_begin)
                   << " earlier points.";
      warned_for_dropped_points = true;
    }

    // Copy overlapping range.
    ///������ڵ�
    if (overlap_begin < overlap_end) {
      std::size_t origin_index = result.origins.size(); ///ԭʼ����vector��С
      result.origins.push_back(data.origin);
      const float time_correction =
          static_cast<float>(common::ToSeconds(data.time - current_end_));
      for (auto overlap_it = overlap_begin; overlap_it != overlap_end;
           ++overlap_it) {
        sensor::TimedPointCloudOriginData::RangeMeasurement point{*overlap_it,
                                                                  origin_index};
        // current_end_ + point_time[3]_after == in_timestamp +
        // point_time[3]_before
        point.point_time.time += time_correction;
        result.ranges.push_back(point);
      }
    }

    // Drop buffered points until overlap_end.
    ///�޳��������еĵ� ʱ����ڽ���ʱ��֮ǰ��
    ///�ô��������������еĵ� ʱ������ ��ǰ����ʱ��
    if (overlap_end == ranges.end()) {
      it = id_to_pending_data_.erase(it); ///��յ��ƻ���
    } else if (overlap_end == ranges.begin()) {
      ++it;
    ///�����ô�����������ʱ����ڵ�ǰ����ʱ��ĵ�
    } else {
      data = sensor::TimedPointCloudData{
          data.time, data.origin,
          sensor::TimedPointCloud(overlap_end, ranges.end())};
      ++it;
    }
  }

  ///std::sort(��ʼ,����,�ȽϺ���) ���ձȽϺ�������true�ķ�ʽ����
  ///�Ը������������еĵ㰴ʱ���С��������
  std::sort(result.ranges.begin(), result.ranges.end(),
            [](const sensor::TimedPointCloudOriginData::RangeMeasurement& a,
               const sensor::TimedPointCloudOriginData::RangeMeasurement& b) {
              return a.point_time.time < b.point_time.time;
            });
  return result;
}

}  // namespace mapping
}  // namespace cartographer
