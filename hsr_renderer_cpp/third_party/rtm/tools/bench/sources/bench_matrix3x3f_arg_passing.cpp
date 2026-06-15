////////////////////////////////////////////////////////////////////////////////
// The MIT License (MIT)
//
// Copyright (c) 2024 Nicholas Frechette & Realtime Math contributors
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

#include <benchmark/benchmark.h>

#include <rtm/matrix3x3f.h>
#include <rtm/quatf.h>

using namespace rtm;

RTM_DISABLE_SECURITY_COOKIE_CHECK RTM_FORCE_NOINLINE matrix3x3f RTM_SIMD_CALL matrix_mul_passing_current(matrix3x3f_arg0 lhs, matrix3x3f_arg1 rhs) RTM_NO_EXCEPT
{
	vector4f tmp = vector_mul(vector_dup_x(lhs.x_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.x_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.x_axis), rhs.z_axis, tmp);
	vector4f x_axis = tmp;

	tmp = vector_mul(vector_dup_x(lhs.y_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.y_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.y_axis), rhs.z_axis, tmp);
	vector4f y_axis = tmp;

	tmp = vector_mul(vector_dup_x(lhs.z_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.z_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.z_axis), rhs.z_axis, tmp);
	vector4f z_axis = tmp;

	return matrix3x3f{ x_axis, y_axis, z_axis };
}

// On ARM64 (Apple clang), the caller places the 3 addresses into registers x0, x1, and x2
// ldp    q0, q1, [x1]
// ldp    q2, q3, [x0]
// fmul.4s v4, v0, v2[0]
// fmla.4s v4, v1, v2[1]
// ldr    q5, [x1, #0x20]
// fmla.4s v4, v5, v2[2]
// fmul.4s v2, v0, v3[0]
// fmla.4s v2, v1, v3[1]
// fmla.4s v2, v5, v3[2]
// ldr    q3, [x0, #0x20]
// fmul.4s v0, v0, v3[0]
// fmla.4s v0, v1, v3[1]
// fmla.4s v0, v5, v3[2]
// stp    q4, q2, [x2]
// str    q0, [x2, #0x20]
// ret
RTM_DISABLE_SECURITY_COOKIE_CHECK RTM_FORCE_NOINLINE void RTM_SIMD_CALL matrix_mul_passing_ref(const matrix3x3f& lhs, const matrix3x3f& rhs, matrix3x3f& out_result) RTM_NO_EXCEPT
{
	vector4f tmp = vector_mul(vector_dup_x(lhs.x_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.x_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.x_axis), rhs.z_axis, tmp);
	vector4f x_axis = tmp;

	tmp = vector_mul(vector_dup_x(lhs.y_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.y_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.y_axis), rhs.z_axis, tmp);
	vector4f y_axis = tmp;

	tmp = vector_mul(vector_dup_x(lhs.z_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.z_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.z_axis), rhs.z_axis, tmp);
	vector4f z_axis = tmp;

	out_result = matrix3x3f{ x_axis, y_axis, z_axis };
}

// On ARM64 (Apple clang), the caller places the vector values in registers v0, v1, v2, v3, v4, v5 and the result is returned in v0, v1, v2
// fmul.4s v6, v3, v0[0]
// fmla.4s v6, v4, v0[1]
// fmla.4s v6, v5, v0[2]
// fmul.4s v7, v3, v1[0]
// fmla.4s v7, v4, v1[1]
// fmla.4s v7, v5, v1[2]
// fmul.4s v3, v3, v2[0]
// fmla.4s v3, v4, v2[1]
// fmla.4s v3, v5, v2[2]
// mov.16b v0, v6
// mov.16b v1, v7
// mov.16b v2, v3
// ret
RTM_DISABLE_SECURITY_COOKIE_CHECK RTM_FORCE_NOINLINE matrix3x3f RTM_SIMD_CALL matrix_mul_passing_value(const matrix3x3f lhs, const matrix3x3f rhs) RTM_NO_EXCEPT
{
	vector4f tmp = vector_mul(vector_dup_x(lhs.x_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.x_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.x_axis), rhs.z_axis, tmp);
	vector4f x_axis = tmp;

	tmp = vector_mul(vector_dup_x(lhs.y_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.y_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.y_axis), rhs.z_axis, tmp);
	vector4f y_axis = tmp;

	tmp = vector_mul(vector_dup_x(lhs.z_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.z_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.z_axis), rhs.z_axis, tmp);
	vector4f z_axis = tmp;

	return matrix3x3f{ x_axis, y_axis, z_axis };
}

static void bm_matrix3x3f_arg_passing_current(benchmark::State& state)
{
	quatf rotation_around_z = quat_from_euler(scalar_deg_to_rad(0.0F), scalar_deg_to_rad(90.0F), scalar_deg_to_rad(0.0F));
	matrix3x3f m0 = matrix_from_quat(rotation_around_z);

	for (auto _ : state)
	{
		// We use the same matrix for input/output to simulate the worst case scenario
		// where we might need store-forwarding to load our inputs
		// In practice, when the function is called, we don't know what produced the inputs
		m0 = matrix_mul_passing_current(m0, m0);
		m0 = matrix_mul_passing_current(m0, m0);
		m0 = matrix_mul_passing_current(m0, m0);
		m0 = matrix_mul_passing_current(m0, m0);
	}

	benchmark::DoNotOptimize(m0);
}

BENCHMARK(bm_matrix3x3f_arg_passing_current);

static void bm_matrix3x3f_arg_passing_ref(benchmark::State& state)
{
	quatf rotation_around_z = quat_from_euler(scalar_deg_to_rad(0.0F), scalar_deg_to_rad(90.0F), scalar_deg_to_rad(0.0F));
	matrix3x3f m0 = matrix_from_quat(rotation_around_z);

	for (auto _ : state)
	{
		// We use the same matrix for input/output to simulate the worst case scenario
		// where we might need store-forwarding to load our inputs
		// In practice, when the function is called, we don't know what produced the inputs
		// Here, we'll populate the input registers with the desired memory addresses which is
		// very cheap but we'll incur memory round-trips and store-forwarding
		matrix_mul_passing_ref(m0, m0, m0);
		matrix_mul_passing_ref(m0, m0, m0);
		matrix_mul_passing_ref(m0, m0, m0);
		matrix_mul_passing_ref(m0, m0, m0);
	}

	benchmark::DoNotOptimize(m0);
}

BENCHMARK(bm_matrix3x3f_arg_passing_ref);

static void bm_matrix3x3f_arg_passing_value(benchmark::State& state)
{
	quatf rotation_around_z = quat_from_euler(scalar_deg_to_rad(0.0F), scalar_deg_to_rad(90.0F), scalar_deg_to_rad(0.0F));
	matrix3x3f m0 = matrix_from_quat(rotation_around_z);

	for (auto _ : state)
	{
		// We use the same matrix for input/output to simulate the worst case scenario
		// where we might need to duplicate input register values
		// In practice, when the function is called, we don't know what produced the inputs
		// Here, we'll populate the input registers with the output register values with 'mov'
		// instructions which is very cheap and we avoid touching memory
		m0 = matrix_mul_passing_value(m0, m0);
		m0 = matrix_mul_passing_value(m0, m0);
		m0 = matrix_mul_passing_value(m0, m0);
		m0 = matrix_mul_passing_value(m0, m0);
	}

	benchmark::DoNotOptimize(m0);
}

BENCHMARK(bm_matrix3x3f_arg_passing_value);
