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

#include "cartographer/mapping/internal/3d/scan_matching/precomputation_grid_3d.h"

#include <algorithm>

#include "Eigen/Core"
#include "cartographer/common/math.h"
#include "cartographer/mapping/probability_values.h"
#include "glog/logging.h"

namespace cartographer {
namespace mapping {
namespace scan_matching {
namespace {

// C++11 defines that integer division rounds towards zero. For index math, we
// actually need it to round towards negative infinity. Luckily bit shifts have
// that property.
///��һ�����ݳ���2
inline int DivideByTwoRoundingTowardsNegativeInfinity(const int value) {
  return value >> 1;
}

// Computes the half resolution index corresponding to the full resolution
// 'cell_index'.
///��3D���ÿһ��ά�ȶ�����2
Eigen::Array3i CellIndexAtHalfResolution(const Eigen::Array3i& cell_index) {
  return Eigen::Array3i(
      DivideByTwoRoundingTowardsNegativeInfinity(cell_index[0]),
      DivideByTwoRoundingTowardsNegativeInfinity(cell_index[1]),
      DivideByTwoRoundingTowardsNegativeInfinity(cell_index[2]));
}

}  // namespace

///������դ���ͼ��0-255����ɢ���ʵ�ͼ��ʾ
PrecomputationGrid3D ConvertToPrecomputationGrid(
    const HybridGrid& hybrid_grid) {
  PrecomputationGrid3D result(hybrid_grid.resolution()); ///ʹ�ø��ʵ�ͼ����һ�����������
  ///���ʵ�ͼ�ڵ�ÿһ�����浥Ԫ
  for (auto it = HybridGrid::Iterator(hybrid_grid); !it.Done(); it.Next()) {
    ///�Ѹ���դ���е�����ĸ��� ��һ����0-255֮�������
    const int cell_value = common::RoundToInt(
        (ValueToProbability(it.GetValue()) - kMinProbability) *
        (255.f / (kMaxProbability - kMinProbability)));
    CHECK_GE(cell_value, 0);
    CHECK_LE(cell_value, 255);
    *result.mutable_value(it.GetCellIndex()) = cell_value; ///�޸�ת����������ֵ
  }
  return result;
}

/*
 * ����ʹ�õĽ���������,��ռ�õĴ���ռ���Ȼ��ԭʼ������ͬ,���Թ���ʹ�õķֱ�����һ����.
 * ֻ����ĳһ���depth��,�߳�Ϊ2^depth���浥Ԫ�������δ���ռ���,ֻ��һ����0-255����
 * �Ҹ����ݴ�������һ��������Ϊ(0,0,0)��λ��,�����߸�λ��û������ �������ڵ�����ʱ����Զ�����
 * ����������񽵲��� ����Ҫ��3D��ų���2
 */

///��һ������� �Ƿ񽵵ͷֱ��� �����ԭʼ������ƶ�
///������һ������� ������һ������� ÿ����Ԫ��ֵ
PrecomputationGrid3D PrecomputeGrid(const PrecomputationGrid3D& grid,
                                    const bool half_resolution,
                                    const Eigen::Array3i& shift) {
  PrecomputationGrid3D result(grid.resolution()); ///�ֱ��ʵ�����һ���0-255����ķֱ���
  ///��һ���0-255�����ÿһ�����浥Ԫ
  for (auto it = PrecomputationGrid3D::Iterator(grid); !it.Done(); it.Next()) {
      ///��һ�����浥Ԫ����(0,0,0) ��������Χ��(0,0,0),(0,0,1)...(1,1,1) 8����Ƚ�
      ///�ҵ���˸��������� ��Ϊ�������ֵ
      ///�����Ҫ���ͷֱ��� �򽫸õ�Ԫ��3D���ÿһ��ά�ȶ�����2
    for (int i = 0; i != 8; ++i) {
      // We use this value to update 8 values in the resulting grid, at
      // position (x - {0, 'shift'}, y - {0, 'shift'}, z - {0, 'shift'}).
      // If 'shift' is 2 ** (depth - 1), where depth 0 is the original grid,
      // this results in precomputation grids analogous to the 2D case.
      const Eigen::Array3i cell_index =
          it.GetCellIndex() - shift * PrecomputationGrid3D::GetOctant(i);
      auto* const cell_value = result.mutable_value(
          half_resolution ? CellIndexAtHalfResolution(cell_index) : cell_index);
      ///һ����Ԫ�ٽ���8����Ԫֻ����һ�����ֵ
      *cell_value = std::max(it.GetValue(), *cell_value);
    }
  }
  return result;
}

}  // namespace scan_matching
}  // namespace mapping
}  // namespace cartographer
