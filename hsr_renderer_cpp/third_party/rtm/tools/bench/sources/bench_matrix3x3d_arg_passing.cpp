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

#include <rtm/matrix3x3d.h>
#include <rtm/quatd.h>

using namespace rtm;

RTM_DISABLE_SECURITY_COOKIE_CHECK RTM_FORCE_NOINLINE matrix3x3d RTM_SIMD_CALL matrix_mul_passing_current(matrix3x3d_arg0 lhs, matrix3x3d_arg1 rhs) RTM_NO_EXCEPT
{
	vector4d tmp = vector_mul(vector_dup_x(lhs.x_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.x_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.x_axis), rhs.z_axis, tmp);
	vector4d x_axis = tmp;

	tmp = vector_mul(vector_dup_x(lhs.y_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.y_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.y_axis), rhs.z_axis, tmp);
	vector4d y_axis = tmp;

	tmp = vector_mul(vector_dup_x(lhs.z_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.z_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.z_axis), rhs.z_axis, tmp);
	vector4d z_axis = tmp;

	return matrix3x3d{ x_axis, y_axis, z_axis };
}

RTM_DISABLE_SECURITY_COOKIE_CHECK RTM_FORCE_NOINLINE void RTM_SIMD_CALL matrix_mul_passing_ref(const matrix3x3d& lhs, const matrix3x3d& rhs, matrix3x3d& out_result) RTM_NO_EXCEPT
{
	vector4d tmp = vector_mul(vector_dup_x(lhs.x_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.x_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.x_axis), rhs.z_axis, tmp);
	vector4d x_axis = tmp;

	tmp = vector_mul(vector_dup_x(lhs.y_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.y_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.y_axis), rhs.z_axis, tmp);
	vector4d y_axis = tmp;

	tmp = vector_mul(vector_dup_x(lhs.z_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.z_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.z_axis), rhs.z_axis, tmp);
	vector4d z_axis = tmp;

	out_result = matrix3x3d{ x_axis, y_axis, z_axis };
}

RTM_DISABLE_SECURITY_COOKIE_CHECK RTM_FORCE_NOINLINE matrix3x3d RTM_SIMD_CALL matrix_mul_passing_value(const matrix3x3d lhs, const matrix3x3d rhs) RTM_NO_EXCEPT
{
	vector4d tmp = vector_mul(vector_dup_x(lhs.x_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.x_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.x_axis), rhs.z_axis, tmp);
	vector4d x_axis = tmp;

	tmp = vector_mul(vector_dup_x(lhs.y_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.y_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.y_axis), rhs.z_axis, tmp);
	vector4d y_axis = tmp;

	tmp = vector_mul(vector_dup_x(lhs.z_axis), rhs.x_axis);
	tmp = vector_mul_add(vector_dup_y(lhs.z_axis), rhs.y_axis, tmp);
	tmp = vector_mul_add(vector_dup_z(lhs.z_axis), rhs.z_axis, tmp);
	vector4d z_axis = tmp;

	return matrix3x3d{ x_axis, y_axis, z_axis };
}

static void bm_matrix3x3d_arg_passing_current(benchmark::State& state)
{
	quatd rotation_around_z = quat_from_euler(scalar_deg_to_rad(0.0), scalar_deg_to_rad(90.0), scalar_deg_to_rad(0.0));
	matrix3x3d m0 = matrix_from_quat(rotation_around_z);

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

BENCHMARK(bm_matrix3x3d_arg_passing_current);

static void bm_matrix3x3d_arg_passing_ref(benchmark::State& state)
{
	quatd rotation_around_z = quat_from_euler(scalar_deg_to_rad(0.0), scalar_deg_to_rad(90.0), scalar_deg_to_rad(0.0));
	matrix3x3d m0 = matrix_from_quat(rotation_around_z);

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

BENCHMARK(bm_matrix3x3d_arg_passing_ref);

static void bm_matrix3x3d_arg_passing_value(benchmark::State& state)
{
	quatd rotation_around_z = quat_from_euler(scalar_deg_to_rad(0.0), scalar_deg_to_rad(90.0), scalar_deg_to_rad(0.0));
	matrix3x3d m0 = matrix_from_quat(rotation_around_z);

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

BENCHMARK(bm_matrix3x3d_arg_passing_value);
