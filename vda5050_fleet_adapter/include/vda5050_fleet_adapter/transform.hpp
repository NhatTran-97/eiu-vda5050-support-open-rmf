#pragma once

#include <array>
#include <cmath>

namespace vda5050_fleet_adapter {

/*
  2D affine transform between the RMF nav-graph frame and a robot map frame:
  robot = scale * R(rotation) * rmf + translation

  Configure per robot in config.yaml. Identity by default (frames equal).
  Ported from the reference RobotClientAPI.py `Transform`.
*/
class Transform
{
public:
  Transform(double rotation = 0.0, double scale = 1.0, double tx = 0.0, double ty = 0.0): _rotation(rotation), _scale(scale), _tx(tx), _ty(ty),
    _c(std::cos(rotation)), _s(std::sin(rotation))
  {
  }

  /// RMF (x, y, theta) -> robot frame.
  std::array<double, 3> to_robot(double x, double y, double theta) const
  {
    const double rx = _scale * (_c * x - _s * y) + _tx;
    const double ry = _scale * (_s * x + _c * y) + _ty;
    return {rx, ry, theta + _rotation};
  }

  /// Robot frame (x, y, theta) -> RMF.
  std::array<double, 3> to_rmf(double x, double y, double theta) const
  {
    const double x0 = (x - _tx) / _scale;
    const double y0 = (y - _ty) / _scale;
    const double rx = _c * x0 + _s * y0;
    const double ry = -_s * x0 + _c * y0;
    return {rx, ry, theta - _rotation};
  }

private:
  double _rotation;
  double _scale;
  double _tx;
  double _ty;
  double _c;
  double _s;
};

}  // namespace vda5050_fleet_adapter
