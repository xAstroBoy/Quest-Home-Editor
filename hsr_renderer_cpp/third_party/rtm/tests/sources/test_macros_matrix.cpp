////////////////////////////////////////////////////////////////////////////////
// The MIT License (MIT)
//
// Copyright (c) 2021 Nicholas Frechette & Realtime Math contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
////////////////////////////////////////////////////////////////////////////////

#include "catch2.impl.h"

#include <rtm/macros.h>
#include <rtm/vector4d.h>
#include <rtm/vector4f.h>

TEST_CASE("macros matrixf", "[math][macros][matrix]")
{
	const float threshold = 0.0F;	// Result must be binary exact!

	{
		rtm::vector4f xy0 = rtm::vector_set(1.0F, 2.0F, 3.0F);
		rtm::vector4f xy1 = rtm::vector_set(4.0F, 5.0F, 6.0F);

		rtm::vector4f xx;
		rtm::vector4f yy;
		RTM_MATRIXF_TRANSPOSE_2X2(xy0, xy1, xx, yy);

		CHECK(rtm::vector_all_near_equal2(rtm::vector_set(1.0F, 4.0F, 7.0F), xx, threshold));
		CHECK(rtm::vector_all_near_equal2(rtm::vector_set(2.0F, 5.0F, 8.0F), yy, threshold));

		// Test when input == output
		RTM_MATRIXF_TRANSPOSE_2X2(xy0, xy1, xy0, xy1);

		CHECK(rtm::vector_all_near_equal2(rtm::vector_set(1.0F, 4.0F, 7.0F), xy0, threshold));
		CHECK(rtm::vector_all_near_equal2(rtm::vector_set(2.0F, 5.0F, 8.0F), xy1, threshold));
	}

	{
		rtm::vector4f xyz0 = rtm::vector_set(1.0F, 2.0F, 3.0F);
		rtm::vector4f xyz1 = rtm::vector_set(4.0F, 5.0F, 6.0F);
		rtm::vector4f xyz2 = rtm::vector_set(7.0F, 8.0F, 9.0F);

		rtm::vector4f xxx;
		rtm::vector4f yyy;
		rtm::vector4f zzz;
		RTM_MATRIXF_TRANSPOSE_3X3(xyz0, xyz1, xyz2, xxx, yyy, zzz);

		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(1.0F, 4.0F, 7.0F), xxx, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(2.0F, 5.0F, 8.0F), yyy, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(3.0F, 6.0F, 9.0F), zzz, threshold));

		// Test when input == output
		RTM_MATRIXF_TRANSPOSE_3X3(xyz0, xyz1, xyz2, xyz0, xyz1, xyz2);

		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(1.0F, 4.0F, 7.0F), xyz0, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(2.0F, 5.0F, 8.0F), xyz1, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(3.0F, 6.0F, 9.0F), xyz2, threshold));
	}

	{
		rtm::vector4f xyzw0 = rtm::vector_set(1.0F, 2.0F, 3.0F, 20.0F);
		rtm::vector4f xyzw1 = rtm::vector_set(4.0F, 5.0F, 6.0F, 21.0F);
		rtm::vector4f xyzw2 = rtm::vector_set(7.0F, 8.0F, 9.0F, 22.0F);
		rtm::vector4f xyzw3 = rtm::vector_set(10.0F, 11.0F, 12.0F, 23.0F);

		rtm::vector4f xxxx;
		rtm::vector4f yyyy;
		rtm::vector4f zzzz;
		rtm::vector4f wwww;
		RTM_MATRIXF_TRANSPOSE_4X4(xyzw0, xyzw1, xyzw2, xyzw3, xxxx, yyyy, zzzz, wwww);

		CHECK(rtm::vector_all_near_equal(rtm::vector_set(1.0F, 4.0F, 7.0F, 10.0F), xxxx, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(2.0F, 5.0F, 8.0F, 11.0F), yyyy, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(3.0F, 6.0F, 9.0F, 12.0F), zzzz, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(20.0F, 21.0F, 22.0F, 23.0F), wwww, threshold));

		// Test when input == output
		RTM_MATRIXF_TRANSPOSE_4X4(xyzw0, xyzw1, xyzw2, xyzw3, xyzw0, xyzw1, xyzw2, xyzw3);

		CHECK(rtm::vector_all_near_equal(rtm::vector_set(1.0F, 4.0F, 7.0F, 10.0F), xyzw0, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(2.0F, 5.0F, 8.0F, 11.0F), xyzw1, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(3.0F, 6.0F, 9.0F, 12.0F), xyzw2, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(20.0F, 21.0F, 22.0F, 23.0F), xyzw3, threshold));
	}

	{
		rtm::vector4f xyz0 = rtm::vector_set(1.0F, 2.0F, 3.0F, 20.0F);
		rtm::vector4f xyz1 = rtm::vector_set(4.0F, 5.0F, 6.0F, 21.0F);
		rtm::vector4f xyz2 = rtm::vector_set(7.0F, 8.0F, 9.0F, 22.0F);
		rtm::vector4f xyz3 = rtm::vector_set(10.0F, 11.0F, 12.0F, 23.0F);

		rtm::vector4f xxxx;
		rtm::vector4f yyyy;
		rtm::vector4f zzzz;
		RTM_MATRIXF_TRANSPOSE_4X3(xyz0, xyz1, xyz2, xyz3, xxxx, yyyy, zzzz);

		CHECK(rtm::vector_all_near_equal(rtm::vector_set(1.0F, 4.0F, 7.0F, 10.0F), xxxx, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(2.0F, 5.0F, 8.0F, 11.0F), yyyy, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(3.0F, 6.0F, 9.0F, 12.0F), zzzz, threshold));

		// Test when input == output
		RTM_MATRIXF_TRANSPOSE_4X3(xyz0, xyz1, xyz2, xyz3, xyz0, xyz1, xyz2);

		CHECK(rtm::vector_all_near_equal(rtm::vector_set(1.0F, 4.0F, 7.0F, 10.0F), xyz0, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(2.0F, 5.0F, 8.0F, 11.0F), xyz1, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(3.0F, 6.0F, 9.0F, 12.0F), xyz2, threshold));
	}

	{
		rtm::vector4f xyzw0 = rtm::vector_set(1.0F, 2.0F, 3.0F, 20.0F);
		rtm::vector4f xyzw1 = rtm::vector_set(4.0F, 5.0F, 6.0F, 21.0F);
		rtm::vector4f xyzw2 = rtm::vector_set(7.0F, 8.0F, 9.0F, 22.0F);

		rtm::vector4f xxx;
		rtm::vector4f yyy;
		rtm::vector4f zzz;
		rtm::vector4f www;
		RTM_MATRIXF_TRANSPOSE_3X4(xyzw0, xyzw1, xyzw2, xxx, yyy, zzz, www);

		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(1.0F, 4.0F, 7.0F), xxx, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(2.0F, 5.0F, 8.0F), yyy, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(3.0F, 6.0F, 9.0F), zzz, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(20.0F, 21.0F, 22.0F), www, threshold));

		// Test when input == output
		RTM_MATRIXF_TRANSPOSE_3X4(xyzw0, xyzw1, xyzw2, xyzw0, xyzw1, xyzw2, xxx);

		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(1.0F, 4.0F, 7.0F), xyzw0, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(2.0F, 5.0F, 8.0F), xyzw1, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(3.0F, 6.0F, 9.0F), xyzw2, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(20.0F, 21.0F, 22.0F), xxx, threshold));
	}
}

TEST_CASE("macros matrixd", "[math][macros][matrix]")
{
	const double threshold = 0.0;	// Result must be binary exact!

	{
		rtm::vector4d xy0 = rtm::vector_set(1.0, 2.0, 3.0);
		rtm::vector4d xy1 = rtm::vector_set(4.0, 5.0, 6.0);

		rtm::vector4d xx;
		rtm::vector4d yy;
		RTM_MATRIXD_TRANSPOSE_2X2(xy0, xy1, xx, yy);

		CHECK(rtm::vector_all_near_equal2(rtm::vector_set(1.0, 4.0, 7.0), xx, threshold));
		CHECK(rtm::vector_all_near_equal2(rtm::vector_set(2.0, 5.0, 8.0), yy, threshold));

		// Test when input == output
		RTM_MATRIXD_TRANSPOSE_2X2(xy0, xy1, xy0, xy1);

		CHECK(rtm::vector_all_near_equal2(rtm::vector_set(1.0, 4.0, 7.0), xy0, threshold));
		CHECK(rtm::vector_all_near_equal2(rtm::vector_set(2.0, 5.0, 8.0), xy1, threshold));
	}

	{
		rtm::vector4d xyz0 = rtm::vector_set(1.0, 2.0, 3.0);
		rtm::vector4d xyz1 = rtm::vector_set(4.0, 5.0, 6.0);
		rtm::vector4d xyz2 = rtm::vector_set(7.0, 8.0, 9.0);

		rtm::vector4d xxx;
		rtm::vector4d yyy;
		rtm::vector4d zzz;
		RTM_MATRIXD_TRANSPOSE_3X3(xyz0, xyz1, xyz2, xxx, yyy, zzz);

		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(1.0, 4.0, 7.0), xxx, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(2.0, 5.0, 8.0), yyy, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(3.0, 6.0, 9.0), zzz, threshold));

		// Test when input == output
		RTM_MATRIXD_TRANSPOSE_3X3(xyz0, xyz1, xyz2, xyz0, xyz1, xyz2);

		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(1.0, 4.0, 7.0), xyz0, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(2.0, 5.0, 8.0), xyz1, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(3.0, 6.0, 9.0), xyz2, threshold));
	}

	{
		rtm::vector4d xyzw0 = rtm::vector_set(1.0, 2.0, 3.0, 20.0);
		rtm::vector4d xyzw1 = rtm::vector_set(4.0, 5.0, 6.0, 21.0);
		rtm::vector4d xyzw2 = rtm::vector_set(7.0, 8.0, 9.0, 22.0);
		rtm::vector4d xyzw3 = rtm::vector_set(10.0, 11.0, 12.0, 23.0);

		rtm::vector4d xxxx;
		rtm::vector4d yyyy;
		rtm::vector4d zzzz;
		rtm::vector4d wwww;
		RTM_MATRIXD_TRANSPOSE_4X4(xyzw0, xyzw1, xyzw2, xyzw3, xxxx, yyyy, zzzz, wwww);

		CHECK(rtm::vector_all_near_equal(rtm::vector_set(1.0, 4.0, 7.0, 10.0), xxxx, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(2.0, 5.0, 8.0, 11.0), yyyy, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(3.0, 6.0, 9.0, 12.0), zzzz, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(20.0, 21.0, 22.0, 23.0), wwww, threshold));

		// Test when input == output
		RTM_MATRIXD_TRANSPOSE_4X4(xyzw0, xyzw1, xyzw2, xyzw3, xyzw0, xyzw1, xyzw2, xyzw3);

		CHECK(rtm::vector_all_near_equal(rtm::vector_set(1.0, 4.0, 7.0, 10.0), xyzw0, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(2.0, 5.0, 8.0, 11.0), xyzw1, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(3.0, 6.0, 9.0, 12.0), xyzw2, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(20.0, 21.0, 22.0, 23.0), xyzw3, threshold));
	}

	{
		rtm::vector4d xyz0 = rtm::vector_set(1.0, 2.0, 3.0, 20.0);
		rtm::vector4d xyz1 = rtm::vector_set(4.0, 5.0, 6.0, 21.0);
		rtm::vector4d xyz2 = rtm::vector_set(7.0, 8.0, 9.0, 22.0);
		rtm::vector4d xyz3 = rtm::vector_set(10.0, 11.0, 12.0, 23.0);

		rtm::vector4d xxxx;
		rtm::vector4d yyyy;
		rtm::vector4d zzzz;
		RTM_MATRIXD_TRANSPOSE_4X3(xyz0, xyz1, xyz2, xyz3, xxxx, yyyy, zzzz);

		CHECK(rtm::vector_all_near_equal(rtm::vector_set(1.0, 4.0, 7.0, 10.0), xxxx, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(2.0, 5.0, 8.0, 11.0), yyyy, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(3.0, 6.0, 9.0, 12.0), zzzz, threshold));

		// Test when input == output
		RTM_MATRIXD_TRANSPOSE_4X3(xyz0, xyz1, xyz2, xyz3, xyz0, xyz1, xyz2);

		CHECK(rtm::vector_all_near_equal(rtm::vector_set(1.0, 4.0, 7.0, 10.0), xyz0, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(2.0, 5.0, 8.0, 11.0), xyz1, threshold));
		CHECK(rtm::vector_all_near_equal(rtm::vector_set(3.0, 6.0, 9.0, 12.0), xyz2, threshold));
	}

	{
		rtm::vector4d xyzw0 = rtm::vector_set(1.0, 2.0, 3.0, 20.0);
		rtm::vector4d xyzw1 = rtm::vector_set(4.0, 5.0, 6.0, 21.0);
		rtm::vector4d xyzw2 = rtm::vector_set(7.0, 8.0, 9.0, 22.0);

		rtm::vector4d xxx;
		rtm::vector4d yyy;
		rtm::vector4d zzz;
		rtm::vector4d www;
		RTM_MATRIXD_TRANSPOSE_3X4(xyzw0, xyzw1, xyzw2, xxx, yyy, zzz, www);

		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(1.0, 4.0, 7.0), xxx, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(2.0, 5.0, 8.0), yyy, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(3.0, 6.0, 9.0), zzz, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(20.0, 21.0, 22.0), www, threshold));

		// Test when input == output
		RTM_MATRIXD_TRANSPOSE_3X4(xyzw0, xyzw1, xyzw2, xyzw0, xyzw1, xyzw2, xxx);

		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(1.0, 4.0, 7.0), xyzw0, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(2.0, 5.0, 8.0), xyzw1, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(3.0, 6.0, 9.0), xyzw2, threshold));
		CHECK(rtm::vector_all_near_equal3(rtm::vector_set(20.0, 21.0, 22.0), xxx, threshold));
	}
}
