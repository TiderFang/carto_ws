/*
 * Copyright 2016 The Cartographer Authors
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

#include "cartographer/mapping/internal/3d/scan_matching/fast_correlative_scan_matcher_3d.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

#include "Eigen/Geometry"
#include "absl/memory/memory.h"
#include "cartographer/common/math.h"
#include "cartographer/mapping/internal/3d/scan_matching/low_resolution_matcher.h"
#include "cartographer/mapping/proto/scan_matching//fast_correlative_scan_matcher_options_3d.pb.h"
#include "cartographer/transform/transform.h"
#include "glog/logging.h"

namespace cartographer {
namespace mapping {
namespace scan_matching {

proto::FastCorrelativeScanMatcherOptions3D
CreateFastCorrelativeScanMatcherOptions3D(
    common::LuaParameterDictionary* const parameter_dictionary) {
  proto::FastCorrelativeScanMatcherOptions3D options;
  ///ʹ�ü������ һ����8
  options.set_branch_and_bound_depth(
      parameter_dictionary->GetInt("branch_and_bound_depth"));
  ///��֤ԭʼ�ֱ��ʵĲ��� һ����3
  options.set_full_resolution_depth(
      parameter_dictionary->GetInt("full_resolution_depth"));
  ///��������ֱ��ͼƥ��Ҫ�����С�÷�
  options.set_min_rotational_score(
      parameter_dictionary->GetDouble("min_rotational_score"));
  options.set_min_low_resolution_score(
      parameter_dictionary->GetDouble("min_low_resolution_score"));
  ///�������ڴ�С
  options.set_linear_xy_search_window(
      parameter_dictionary->GetDouble("linear_xy_search_window"));
  options.set_linear_z_search_window(
      parameter_dictionary->GetDouble("linear_z_search_window"));
  options.set_angular_search_window(
      parameter_dictionary->GetDouble("angular_search_window"));
  return options;
}

/*
 * ����ʹ�õĽ���������,��ռ�õĴ���ռ���Ȼ��ԭʼ������ͬ,���Թ���ʹ�õķֱ�����һ����.
 * ֻ����ĳһ���depth��,�߳�Ϊ2^depth���浥Ԫ�������δ���ռ���,ֻ��һ����0-255����
 * �Ҹ����ݴ�������һ��������Ϊ(0,0,0)��λ��,�����߸�λ��û������ �������ڵ�����ʱ����Զ�����
 * ����������񽵲��� ����Ҫ��3D��ų���2
 */

///����: ���ʸ�դ��ͼ ����
///�����ʸ�դ��ͼת��Ϊ0-255�÷ֵ�ͼ
PrecomputationGridStack3D::PrecomputationGridStack3D(
    const HybridGrid& hybrid_grid,
    const proto::FastCorrelativeScanMatcherOptions3D& options) {
  CHECK_GE(options.branch_and_bound_depth(), 1);
  CHECK_GE(options.full_resolution_depth(), 1);
  ///0-255��ɢ��ͼ�б�ĵ�һ����ͼ�Ǹ��ʵ�ͼ��0-255��ͼ
  precomputation_grids_.reserve(options.branch_and_bound_depth());
  precomputation_grids_.push_back(ConvertToPrecomputationGrid(hybrid_grid));
  Eigen::Array3i last_width = Eigen::Array3i::Ones();
  for (int depth = 1; depth != options.branch_and_bound_depth(); ++depth) {
    const bool half_resolution = depth >= options.full_resolution_depth(); ///�Ƿ񽵵ͷֱ���
    ///depth����3 next_width = (8,8,8)����
    const Eigen::Array3i next_width = ((1 << depth) * Eigen::Array3i::Ones()); ///����������С
    const int full_voxels_per_high_resolution_voxel = ///ÿ������ķֱ���
        1 << std::max(0, depth - options.full_resolution_depth());
    ///������Ⱥͷֱ��� ���㵱ǰ����(0,0,0)-(1,1,1)�����λ�ƴ�С
    const Eigen::Array3i shift = (next_width - last_width +
                                  (full_voxels_per_high_resolution_voxel - 1)) /
                                 full_voxels_per_high_resolution_voxel;
    precomputation_grids_.push_back(
        PrecomputeGrid(precomputation_grids_.back(), half_resolution, shift));
    last_width = next_width;
  }
}

///��ɢ��ĵ���
struct DiscreteScan3D {
  transform::Rigid3f pose; ///��ǰ֡�����һ֡���ܵ�λ�˱任
  // Contains a vector of discretized scans for each 'depth'.
  std::vector<std::vector<Eigen::Array3i>> cell_indices_per_depth; ///�����еĵ���ÿһ�����0-255��ͼ�е�3D���
  float rotational_score; ///��ת�÷�
};

///��ѡλ��
struct Candidate3D {
  Candidate3D(const int scan_index, const Eigen::Array3i& offset)
      : scan_index(scan_index), offset(offset) {}

  static Candidate3D Unsuccessful() {
    return Candidate3D(0, Eigen::Array3i::Zero());
  }

  // Index into the discrete scans vectors.
  int scan_index; ///ֱ��ͼƥ��ɹ�����ɢ���� һά��� ��ʾ�ڼ�����ת

  // Linear offset from the initial pose in cell indices. For lower resolution
  // candidates this is the lowest offset of the 2^depth x 2^depth x 2^depth
  // block of possibilities.
  //��Ԫ�����������ʼ���Ƶ�����ƫ�ơ����ڵͷֱ��ʺ�ѡ������2^depth x 2^depth x 2^depth������Ե���Сƫ������
  Eigen::Array3i offset; ///��������ƫ��

  // Score, higher is better.
  // ��������ֵ��������󡱣��ɸ�������T��ʾ��
  // ����std����numeric_limits<T>����has_infinity==trueʱ�������塣
  float score = -std::numeric_limits<float>::infinity();

  // Score of the low resolution matcher.
  float low_resolution_score = 0.f; ///�ú�ѡλ���ڵͷֱ��ʺ;ֲ���ͼ��ƥ��÷�

  bool operator<(const Candidate3D& other) const { return score < other.score; }
  bool operator>(const Candidate3D& other) const { return score > other.score; }
};

///��դ �ͷֱ��ʸ�դ ��תɨ��ƥ��ֱ��ͼ ����
FastCorrelativeScanMatcher3D::FastCorrelativeScanMatcher3D(
    const HybridGrid& hybrid_grid,
    const HybridGrid* const low_resolution_hybrid_grid,
    const Eigen::VectorXf* rotational_scan_matcher_histogram,
    const proto::FastCorrelativeScanMatcherOptions3D& options)
    : options_(options),
      resolution_(hybrid_grid.resolution()),
      width_in_voxels_(hybrid_grid.grid_size()),
      precomputation_grid_stack_(
          absl::make_unique<PrecomputationGridStack3D>(hybrid_grid, options)), //��hybrid_grid, options�����ָ��
      low_resolution_hybrid_grid_(low_resolution_hybrid_grid),
      rotational_scan_matcher_(rotational_scan_matcher_histogram) {}

FastCorrelativeScanMatcher3D::~FastCorrelativeScanMatcher3D() {}

///��һ֡λ�� �ֲ���ͼλ�� ���� �ֲ���λ��С�÷�
std::unique_ptr<FastCorrelativeScanMatcher3D::Result>
FastCorrelativeScanMatcher3D::Match(
    const transform::Rigid3d& global_node_pose,
    const transform::Rigid3d& global_submap_pose,
    const TrajectoryNode::Data& constant_data, const float min_score) const {
    ///low_resolution_matcher���β���pose�ĺ���
    ///�ú���: ʹ��λ�˱任�����еĵ� �����ڵͷֱ���դ���ͼ�������и��ʵĵ���� ��Ϊ�ͷֱ���ƥ��÷�
  const auto low_resolution_matcher = scan_matching::CreateLowResolutionMatcher(
      low_resolution_hybrid_grid_, &constant_data.low_resolution_point_cloud);
  const SearchParameters search_parameters{
      common::RoundToInt(options_.linear_xy_search_window() / resolution_),
      common::RoundToInt(options_.linear_z_search_window() / resolution_),
      options_.angular_search_window(), &low_resolution_matcher};
  ///.cast<flaot>() ǿ������ת��
  return MatchWithSearchParameters(
      search_parameters, global_node_pose.cast<float>(),
      global_submap_pose.cast<float>(),
      constant_data.high_resolution_point_cloud,
      constant_data.rotational_scan_matcher_histogram,
      constant_data.gravity_alignment, min_score);
}

std::unique_ptr<FastCorrelativeScanMatcher3D::Result>
FastCorrelativeScanMatcher3D::MatchFullSubmap(
    const Eigen::Quaterniond& global_node_rotation,
    const Eigen::Quaterniond& global_submap_rotation,
    const TrajectoryNode::Data& constant_data, const float min_score) const {
  float max_point_distance = 0.f;
  for (const sensor::RangefinderPoint& point :
       constant_data.high_resolution_point_cloud) {
    max_point_distance = std::max(max_point_distance, point.position.norm());
  }
  const int linear_window_size =
      (width_in_voxels_ + 1) / 2 +
      common::RoundToInt(max_point_distance / resolution_ + 0.5f);
  const auto low_resolution_matcher = scan_matching::CreateLowResolutionMatcher(
      low_resolution_hybrid_grid_, &constant_data.low_resolution_point_cloud);
  const SearchParameters search_parameters{
      linear_window_size, linear_window_size, M_PI, &low_resolution_matcher};
  return MatchWithSearchParameters(
      search_parameters,
      transform::Rigid3f::Rotation(global_node_rotation.cast<float>()),
      transform::Rigid3f::Rotation(global_submap_rotation.cast<float>()),
      constant_data.high_resolution_point_cloud,
      constant_data.rotational_scan_matcher_histogram,
      constant_data.gravity_alignment, min_score);
}

/////�������� ��һ֡λ�� �ֲ���ͼλ�� �߷ֱ��ʵ��� ��תֱ��ͼ �������� �ֲ���λ��С�÷�
std::unique_ptr<FastCorrelativeScanMatcher3D::Result>
FastCorrelativeScanMatcher3D::MatchWithSearchParameters(
    const FastCorrelativeScanMatcher3D::SearchParameters& search_parameters,
    const transform::Rigid3f& global_node_pose,
    const transform::Rigid3f& global_submap_pose,
    const sensor::PointCloud& point_cloud,
    const Eigen::VectorXf& rotational_scan_matcher_histogram,
    const Eigen::Quaterniond& gravity_alignment, const float min_score) const {
  const std::vector<DiscreteScan3D> discrete_scans = GenerateDiscreteScans(
    ///�ҵ�ֱ��ͼƥ��ȸߵ� ��תλ�˱任
    ///ÿһ��λ�˹���һ��DiscreteScan3D��
      search_parameters, point_cloud, rotational_scan_matcher_histogram,
      gravity_alignment, global_node_pose, global_submap_pose);

  ///�ҵ��ͷֱ����µĺ�ѡλ�� ������
  const std::vector<Candidate3D> lowest_resolution_candidates =
      ComputeLowestResolutionCandidates(search_parameters, discrete_scans);

  ///���ź�ѡλ��
  const Candidate3D best_candidate = BranchAndBound(
      search_parameters, discrete_scans, lowest_resolution_candidates,
      precomputation_grid_stack_->max_depth(), min_score);
  if (best_candidate.score > min_score) {
    return absl::make_unique<Result>(Result{
        best_candidate.score,
        GetPoseFromCandidate(discrete_scans, best_candidate).cast<double>(),
        discrete_scans[best_candidate.scan_index].rotational_score,
        best_candidate.low_resolution_score});
  }
  return nullptr;
}

///�������� ��ǰ֡���� ��ǰ֡�����һ֡���ܵ�λ�˱任 �÷�
///����任�������0-255��ͼ��ÿ����ȵ�λ����� ������һ��DiscreteScan3D��
DiscreteScan3D FastCorrelativeScanMatcher3D::DiscretizeScan(
    const FastCorrelativeScanMatcher3D::SearchParameters& search_parameters,
    const sensor::PointCloud& point_cloud, const transform::Rigid3f& pose,
    const float rotational_score) const {
  std::vector<std::vector<Eigen::Array3i>> cell_indices_per_depth; ///�任�����ÿ�����0-255��ͼ��3D���
  const PrecomputationGrid3D& original_grid = ///ԭʼ�ֲ���ͼ
      precomputation_grid_stack_->Get(0);
  std::vector<Eigen::Array3i> full_resolution_cell_indices;
  ///����ת����ĵ��Ƶĵ� ������ת��Ϊ��ɢ���(�����и�)
  for (const sensor::RangefinderPoint& point :
       sensor::TransformPointCloud(point_cloud, pose)) {
    full_resolution_cell_indices.push_back(
        original_grid.GetCellIndex(point.position));
  }
  const int full_resolution_depth = std::min(options_.full_resolution_depth(), ///���ֱַ��ʵ����
                                             options_.branch_and_bound_depth());
  CHECK_GE(full_resolution_depth, 1);
  ///�ֱ��ʲ��� �������겻��
  for (int i = 0; i != full_resolution_depth; ++i) {
    cell_indices_per_depth.push_back(full_resolution_cell_indices);
  }
  ///�����ֱ��ʵ���Ȳ���
  const int low_resolution_depth =
      options_.branch_and_bound_depth() - full_resolution_depth;
  CHECK_GE(low_resolution_depth, 0);
  ///������������ ������
  const Eigen::Array3i search_window_start(
      -search_parameters.linear_xy_window_size,
      -search_parameters.linear_xy_window_size,
      -search_parameters.linear_z_window_size);
  ///����ÿһ���ͷֱ������
  for (int i = 0; i != low_resolution_depth; ++i) {
    const int reduction_exponent = i + 1;
    const Eigen::Array3i low_resolution_search_window_start( ///���ź���������(������)
        search_window_start[0] >> reduction_exponent,
        search_window_start[1] >> reduction_exponent,
        search_window_start[2] >> reduction_exponent);
    cell_indices_per_depth.emplace_back();
    ///ÿһ��ת��������еĵ���ԭʼ��դ�ĵ�ͼ�е�3D���
    for (const Eigen::Array3i& cell_index : full_resolution_cell_indices) {
      const Eigen::Array3i cell_at_start = cell_index + search_window_start; ///�����еĵ��Ʊ��ֻ�и�������
      ///���ŵ�������
      const Eigen::Array3i low_resolution_cell_at_start(
          cell_at_start[0] >> reduction_exponent,
          cell_at_start[1] >> reduction_exponent,
          cell_at_start[2] >> reduction_exponent);
      ///�����Ʊ���������� ��ӵ�vector(�任�����ÿ�����0-255��ͼ��3D���)
      cell_indices_per_depth.back().push_back(
          low_resolution_cell_at_start - low_resolution_search_window_start);
    }
  }
  ///������ɢ��ĵ���
  return DiscreteScan3D{pose, cell_indices_per_depth, rotational_score};
}

///��ʹ�ÿ��ܵĽǶ�ƥ�� ��ǰ���ƺ;ֲ���ͼƥֱ��ͼ
///����ֱ��ͼƥ��÷ֽϸߵĵ��� ��任������ԭʼ��դ�ĵ�ͼ��ÿ����ȵ�λ�����
///ÿһ��λ�˹���һ��DiscreteScan3D��
///����: �������� ���� ��תֱ��ͼ �������� ��һ֡λ�� �ֲ���ͼλ��
///���: ����ƥ���λ��vector<��ɢ�������(���λ�˱任,�任�������0-255��ͼ��ÿ����ȵ�λ�����,��ת�÷�)>
std::vector<DiscreteScan3D> FastCorrelativeScanMatcher3D::GenerateDiscreteScans(
    const FastCorrelativeScanMatcher3D::SearchParameters& search_parameters,
    const sensor::PointCloud& point_cloud,
    const Eigen::VectorXf& rotational_scan_matcher_histogram,
    const Eigen::Quaterniond& gravity_alignment,
    const transform::Rigid3f& global_node_pose,
    const transform::Rigid3f& global_submap_pose) const {
  std::vector<DiscreteScan3D> result; ///����ƥ���λ��vector<��ɢ�������>
  // We set this value to something on the order of resolution to make sure that
  // the std::acos() below is defined.
  float max_scan_range = 3.f * resolution_;
  for (const sensor::RangefinderPoint& point : point_cloud) {
    const float range = point.position.norm();
    max_scan_range = std::max(range, max_scan_range);
  }
  const float kSafetyMargin = 1.f - 1e-2f;
  const float angular_step_size = ///�Ƕ���������
      kSafetyMargin * std::acos(1.f - common::Pow2(resolution_) /
                                          (2.f * common::Pow2(max_scan_range)));
  const int angular_window_size = common::RoundToInt( ///ͬһ������ĽǶ���������
      search_parameters.angular_search_window / angular_step_size);
  std::vector<float> angles; ///�Ƕ������б�
  for (int rz = -angular_window_size; rz <= angular_window_size; ++rz) {
    angles.push_back(rz * angular_step_size);
  }
  ///�ֲ���ͼ����һ֮֡���λ�˱任
  const transform::Rigid3f node_to_submap =
      global_submap_pose.inverse() * global_node_pose;
  ///����ƥ�����нǶȵĵ�ǰֱ֡��ͼ�;ֲ���ͼ��ֱ��ͼ
  ///�����Ƕ�ֱ��ͼ�����;ֲ���ͼֱ��ͼ�����ļн�cosֵ vector
  const std::vector<float> scores = rotational_scan_matcher_.Match(
      rotational_scan_matcher_histogram,
      transform::GetYaw(node_to_submap.rotation() *
                        gravity_alignment.inverse().cast<float>()),
      angles);
  for (size_t i = 0; i != angles.size(); ++i) {
      ///�����÷�С�����Ҫ��ĵ÷�
    if (scores[i] < options_.min_rotational_score()) {
      continue;
    }
    ///����ֱ��ͼƥ��ɹ��ĵ�ǰ֡λ��
    const Eigen::Vector3f angle_axis(0.f, 0.f, angles[i]);
    // It's important to apply the 'angle_axis' rotation between the translation
    // and rotation of the 'initial_pose', so that the rotation is around the
    // origin of the range data, and yaw is in map frame.
    const transform::Rigid3f pose(
        node_to_submap.translation(),
        global_submap_pose.rotation().inverse() *
            transform::AngleAxisVectorToRotationQuaternion(angle_axis) *
            global_node_pose.rotation());
    result.push_back(
        ///����任�������0-255��ͼ��ÿ����ȵ�λ����� ������һ��DiscreteScan3D��
        DiscretizeScan(search_parameters, point_cloud, pose, scores[i]));
  }
  return result;
}

///����ͷֱ��ʺ�ѡλ���б�
///����: �������� λ�˸���
///���: vector<��תλ�� ��������������>
std::vector<Candidate3D>
FastCorrelativeScanMatcher3D::GenerateLowestResolutionCandidates(
    const FastCorrelativeScanMatcher3D::SearchParameters& search_parameters,
    const int num_discrete_scans) const {
  const int linear_step_size = 1 << precomputation_grid_stack_->max_depth(); ///������������������
  ///xyz�������������
  ///�����������ǲ��� ʵ����Ӧ������ ���� +���� -���� ��������
  const int num_lowest_resolution_linear_xy_candidates =
      (2 * search_parameters.linear_xy_window_size + linear_step_size) /
      linear_step_size;
  const int num_lowest_resolution_linear_z_candidates =
      (2 * search_parameters.linear_z_window_size + linear_step_size) /
      linear_step_size;
  ///�ܹ��ͷֱ�����������
  const int num_candidates =
      num_discrete_scans *
      common::Power(num_lowest_resolution_linear_xy_candidates, 2) *
      num_lowest_resolution_linear_z_candidates;
  std::vector<Candidate3D> candidates;
  candidates.reserve(num_candidates);
  ///ÿһ����ѡλ��
  for (int scan_index = 0; scan_index != num_discrete_scans; ++scan_index) {
    for (int z = -search_parameters.linear_z_window_size;
         z <= search_parameters.linear_z_window_size; z += linear_step_size) {
      for (int y = -search_parameters.linear_xy_window_size;
           y <= search_parameters.linear_xy_window_size;
           y += linear_step_size) {
        for (int x = -search_parameters.linear_xy_window_size;
             x <= search_parameters.linear_xy_window_size;
             x += linear_step_size) {
          candidates.emplace_back(scan_index, Eigen::Array3i(x, y, z));
        }
      }
    }
  }
  CHECK_EQ(candidates.size(), num_candidates);
  return candidates;
}

///���ݷֱ���ƽ�ƶ�Ӧ��ת���� ������ƽ�ƺ�����ھֲ���ͼ�еĵ÷����
///���ո��ʴӴ������ѡλ��
///����:��� ��ɢ�����б� ��ѡλ���б�
///����:���ݸ��������ĺ�ѡλ���б�
void FastCorrelativeScanMatcher3D::ScoreCandidates(
    const int depth, const std::vector<DiscreteScan3D>& discrete_scans,
    std::vector<Candidate3D>* const candidates) const {
  const int reduction_exponent = ///����ȷֱ���
      std::max(0, depth - options_.full_resolution_depth() + 1);
  ///ÿһ����ѡλ��
  for (Candidate3D& candidate : *candidates) {
    int sum = 0; ///����ƽ�ƺ�ֲ���ͼ�ڵ���λ���ϵĵ÷�
    const DiscreteScan3D& discrete_scan = discrete_scans[candidate.scan_index]; ///��λ�˶�Ӧ����ת��ĵ���
    ///����λ�Ƴ�������ϵ��
    const Eigen::Array3i offset(candidate.offset[0] >> reduction_exponent,
                                candidate.offset[1] >> reduction_exponent,
                                candidate.offset[2] >> reduction_exponent);
    CHECK_LT(depth, discrete_scan.cell_indices_per_depth.size());
    ///�������ÿһ�����3D���(�����и� �Ѿ�����)
    for (const Eigen::Array3i& cell_index :
         discrete_scan.cell_indices_per_depth[depth]) {
      const Eigen::Array3i proposed_cell_index = cell_index + offset; ///ƽ�Ƶ���
      sum += precomputation_grid_stack_->Get(depth).value(proposed_cell_index);
    }
    ///�÷�ת��Ϊ����
    candidate.score = PrecomputationGrid3D::ToProbability(
        sum /
        static_cast<float>(discrete_scan.cell_indices_per_depth[depth].size()));
  }
  ///�Ӵ�С����
  std::sort(candidates->begin(), candidates->end(),
            std::greater<Candidate3D>());
}

///�ҵ��ͷֱ����µĺ�ѡλ�� ������
///����:�������� ��ѡλ��DiscreteScan3D
///���:���ݵͷֱ����µĸ��������ĺ�ѡλ���б�
std::vector<Candidate3D>
FastCorrelativeScanMatcher3D::ComputeLowestResolutionCandidates(
    const FastCorrelativeScanMatcher3D::SearchParameters& search_parameters,
    const std::vector<DiscreteScan3D>& discrete_scans) const {
  ///����ͷֱ��ʺ�ѡλ���б�
  std::vector<Candidate3D> lowest_resolution_candidates = ///��ѡλ���б�
      GenerateLowestResolutionCandidates(search_parameters,
                                         discrete_scans.size());
  ///���ݷֱ���ƽ�ƶ�Ӧ��ת���� ������ƽ�ƺ�����ھֲ���ͼ�еĵ÷����
  ///���ո��ʴӴ������ѡλ��
  ScoreCandidates(precomputation_grid_stack_->max_depth(), discrete_scans,
                  &lowest_resolution_candidates);
  return lowest_resolution_candidates;
}

///��ȡ��ѡλ��
transform::Rigid3f FastCorrelativeScanMatcher3D::GetPoseFromCandidate(
    const std::vector<DiscreteScan3D>& discrete_scans,
    const Candidate3D& candidate) const {
  return transform::Rigid3f::Translation(
             resolution_ * candidate.offset.matrix().cast<float>()) *
         discrete_scans[candidate.scan_index].pose;
}

///����:�������� ��ɢ���� �ͷֱ��ʺ�ѡλ�� ������ �ֲ���λ��С�÷�
///���:���ű任λ��
Candidate3D FastCorrelativeScanMatcher3D::BranchAndBound(
    const FastCorrelativeScanMatcher3D::SearchParameters& search_parameters,
    const std::vector<DiscreteScan3D>& discrete_scans,
    const std::vector<Candidate3D>& candidates, const int candidate_depth,
    float min_score) const {
    ///��������ȵ���0
  if (candidate_depth == 0) {
      ///ÿһ���ͷֱ��ʺ�ѡλ��
    for (const Candidate3D& candidate : candidates) {
        ///����÷ֽ�С ֱ�ӷ��ز�����
      if (candidate.score <= min_score) {
        // Return if the candidate is bad because the following candidate will
        // not have better score.
        return Candidate3D::Unsuccessful();
      }
      const float low_resolution_score = ///�ͷֱ���ƽ�ƺ��ֱ��ͼƥ��÷�
          (*search_parameters.low_resolution_matcher)(
              GetPoseFromCandidate(discrete_scans, candidate));
      ///���ƽ�ƺ��ֱ��ͼƥ��ķ����ɴ�����ֵ
      if (low_resolution_score >= options_.min_low_resolution_score()) {
        // We found the best candidate that passes the matching function.
        Candidate3D best_candidate = candidate;
        best_candidate.low_resolution_score = low_resolution_score;
        return best_candidate;
      }
    }

    // All candidates have good scores but none passes the matching function.
    return Candidate3D::Unsuccessful();
  }

  ///��������Ȳ�����0
  Candidate3D best_high_resolution_candidate = Candidate3D::Unsuccessful(); ///���λ��
  best_high_resolution_candidate.score = min_score; ///�÷�
  ///ÿһ����ѡλ��
  for (const Candidate3D& candidate : candidates) {
      ///����÷�С�ھֲ���λ��С�÷־�����ѭ��(���ں�ѡλ���ǰ��յ÷������)
    if (candidate.score <= min_score) {
      break;
    }

    std::vector<Candidate3D> higher_resolution_candidates; ///����Ⱥ�ѡλ��
    const int half_width = 1 << (candidate_depth - 1); ///����������ȵ�һ��
    ///z��ȡֵֻ���� 0��half_width
      ///�޶������������Χ����������
    for (int z : {0, half_width}) {
      if (candidate.offset.z() + z > search_parameters.linear_z_window_size) {
        break;
      }
      for (int y : {0, half_width}) {
        if (candidate.offset.y() + y >
            search_parameters.linear_xy_window_size) {
          break;
        }
        for (int x : {0, half_width}) {
          if (candidate.offset.x() + x >
              search_parameters.linear_xy_window_size) {
            break;
          }
          higher_resolution_candidates.emplace_back(
              candidate.scan_index, candidate.offset + Eigen::Array3i(x, y, z));
        }
      }
    }
    ///ȡ�÷�����Ҫ��� ������һ�������
    ScoreCandidates(candidate_depth - 1, discrete_scans,
                    &higher_resolution_candidates);
    ///������������е÷����ĺ�ѡλ��
    best_high_resolution_candidate = std::max(
        best_high_resolution_candidate,
        BranchAndBound(search_parameters, discrete_scans,
                       higher_resolution_candidates, candidate_depth - 1,
                       best_high_resolution_candidate.score));
  }
  return best_high_resolution_candidate;
}

}  // namespace scan_matching
}  // namespace mapping
}  // namespace cartographer
