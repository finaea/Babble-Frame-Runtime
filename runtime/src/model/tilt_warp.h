#pragma once

#include "model/image_ops.h"

namespace babble::img {

// Stretches src to dst and turns it counter-clockwise by deg degrees about the centre in one bilinear warp, as
// Baballonia's any-angle rotation does. The turned picture is scaled by 1 / (|cos| + |sin|), so no corner of src is
// cut, and dst pixels outside it are 0, as OpenCV's warpAffine leaves them. At 0 degrees use resize_linear instead,
// which matches OpenCV bit for bit.
void tilt_resize(View src, Mut dst, float deg);

}
