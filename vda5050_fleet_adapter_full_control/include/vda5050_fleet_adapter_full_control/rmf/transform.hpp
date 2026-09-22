#ifndef TRANSFORM_HPP
#define TRANSFORM_HPP

#include <array>
#include <cmath>

namespace vda5050_fleet_adapter_full_control::rmf {

// Transform poses between the RMF graph frame and a robot's map frame.
class Transform
{
public:
    explicit Transform(double rotation = 0.0, double scale = 1.0, double tx = 0.0, double ty = 0.0) : _rotation(rotation), _scale(scale), _tx(tx), _ty(ty),
                                                                                            _c(std::cos(rotation)), _s(std::sin(rotation))
    {
    }

    double rotation() const { return _rotation; }
    double scale() const { return _scale; }
    double tx() const { return _tx; }
    double ty() const { return _ty; }

    // Convert an RMF pose to the robot frame.
    std::array<double, 3> to_robot(double x, double y, double theta) const
    {
        const double rx = _scale * (_c * x - _s * y) + _tx;
        const double ry = _scale * (_s * x + _c * y) + _ty;
        return {rx, ry, wrap(theta + _rotation)};
    }

    // Convert a robot pose to the RMF frame.
    std::array<double, 3> to_rmf(double x, double y, double theta) const
    {
        const double x0 = (x - _tx) / _scale;
        const double y0 = (y - _ty) / _scale;
        const double rx = _c * x0 + _s * y0;
        const double ry = -_s * x0 + _c * y0;
        return {rx, ry, wrap(theta - _rotation)};
    }

private:
    // Normalize heading to the range [-pi, pi).
    static double wrap(double theta)
    {
        constexpr double kPi = 3.14159265358979323846;
        theta = std::fmod(theta + kPi, 2.0 * kPi);
        if (theta < 0.0)
        {
            theta += 2.0 * kPi;
        }
        return theta - kPi;
    }

    double _rotation;
    double _scale;
    double _tx;
    double _ty;
    double _c;
    double _s;
};

}  // namespace vda5050_fleet_adapter_full_control::rmf

#endif  // TRANSFORM_HPP
