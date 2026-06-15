////////////////////////////////////////////////////////////////////////////////
// The MIT License (MIT)
//
// Copyright (c) 2025 Nicholas Frechette & Realtime Math contributors
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

#include <rtm/scalard.h>
#include <rtm/scalarf.h>
#include <rtm/vector4d.h>
#include <rtm/vector4f.h>

using namespace rtm;

TEST_CASE("vector4f math cast", "[math][vector4]")
{
	const vector4f src = vector_set(-2.65F, 2.996113F, 0.68123521F, -5.9182F);
	const vector4d dst = vector_cast(src);
	CHECK(scalar_near_equal(vector_get_x(dst), -2.65, 1.0E-6));
	CHECK(scalar_near_equal(vector_get_y(dst), 2.996113, 1.0E-6));
	CHECK(scalar_near_equal(vector_get_z(dst), 0.68123521, 1.0E-6));
	CHECK(scalar_near_equal(vector_get_w(dst), -5.9182, 1.0E-6));

	const vector4f large_values = vector_set(1073741824.5F, 1073741824.5F, -1073741824.5F, -1073741824.5F);
	CHECK(float(vector_get_x(vector_floor(large_values))) == scalar_floor(float(vector_get_x(large_values))));
	CHECK(float(vector_get_y(vector_floor(large_values))) == scalar_floor(float(vector_get_y(large_values))));
	CHECK(float(vector_get_z(vector_floor(large_values))) == scalar_floor(float(vector_get_z(large_values))));
	CHECK(float(vector_get_w(vector_floor(large_values))) == scalar_floor(float(vector_get_w(large_values))));

	CHECK(float(vector_get_x(vector_ceil(large_values))) == scalar_ceil(float(vector_get_x(large_values))));
	CHECK(float(vector_get_y(vector_ceil(large_values))) == scalar_ceil(float(vector_get_y(large_values))));
	CHECK(float(vector_get_z(vector_ceil(large_values))) == scalar_ceil(float(vector_get_z(large_values))));
	CHECK(float(vector_get_w(vector_ceil(large_values))) == scalar_ceil(float(vector_get_w(large_values))));
}

TEST_CASE("vector4d math cast", "[math][vector4]")
{
	const vector4d src = vector_set(-2.65, 2.996113, 0.68123521, -5.9182);
	const vector4f dst = vector_cast(src);
	CHECK(scalar_near_equal(vector_get_x(dst), -2.65F, 1.0E-6F));
	CHECK(scalar_near_equal(vector_get_y(dst), 2.996113F, 1.0E-6F));
	CHECK(scalar_near_equal(vector_get_z(dst), 0.68123521F, 1.0E-6F));
	CHECK(scalar_near_equal(vector_get_w(dst), -5.9182F, 1.0E-6F));

	const vector4d large_values = vector_set(36028797018963968.5, 36028797018963968.5, -36028797018963968.5, -36028797018963968.5);
	CHECK(double(vector_get_x(vector_floor(large_values))) == scalar_floor(double(vector_get_x(large_values))));
	CHECK(double(vector_get_y(vector_floor(large_values))) == scalar_floor(double(vector_get_y(large_values))));
	CHECK(double(vector_get_z(vector_floor(large_values))) == scalar_floor(double(vector_get_z(large_values))));
	CHECK(double(vector_get_w(vector_floor(large_values))) == scalar_floor(double(vector_get_w(large_values))));

	CHECK(double(vector_get_x(vector_ceil(large_values))) == scalar_ceil(double(vector_get_x(large_values))));
	CHECK(double(vector_get_y(vector_ceil(large_values))) == scalar_ceil(double(vector_get_y(large_values))));
	CHECK(double(vector_get_z(vector_ceil(large_values))) == scalar_ceil(double(vector_get_z(large_values))));
	CHECK(double(vector_get_w(vector_ceil(large_values))) == scalar_ceil(double(vector_get_w(large_values))));
}
