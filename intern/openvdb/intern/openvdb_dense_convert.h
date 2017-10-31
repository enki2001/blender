/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2015 Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Kevin Dietrich
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#ifndef __OPENVDB_DENSE_CONVERT_H__
#define __OPENVDB_DENSE_CONVERT_H__

#include "openvdb_reader.h"
#include "openvdb_writer.h"

#include <openvdb/tools/Clip.h>
#include <openvdb/tools/Dense.h>

#include <cstdio>

namespace internal {

/* Verify that the name does not correspond to the old format, in which case we
 * need to replace the '_low' ending with ' low'. See T53802. */
openvdb::Name do_name_versionning(const openvdb::Name &name);

openvdb::Mat4R convertMatrix(const float mat[4][4]);

template <typename GridType, typename T>
GridType *OpenVDB_export_grid(
        OpenVDBWriter *writer,
        const openvdb::Name &name,
        const T *data,
        const int res[3],
		float fluid_mat[4][4],
		const float clipping,
        const openvdb::FloatGrid *mask)
{
	using namespace openvdb;

	math::CoordBBox bbox(Coord(0), Coord(res[0] - 1, res[1] - 1, res[2] - 1));
	Mat4R mat = convertMatrix(fluid_mat);
	math::Transform::Ptr transform = math::Transform::createLinearTransform(mat);

	typename GridType::Ptr grid = GridType::create(T(0));

	tools::Dense<const T, openvdb::tools::LayoutXYZ> dense_grid(bbox, data);
	tools::copyFromDense(dense_grid, grid->tree(), static_cast<T>(clipping));

	grid->setTransform(transform);

	/* Avoid clipping against an empty grid. */
	if (mask && !mask->tree().empty()) {
		grid = tools::clip(*grid, *mask);
	}

	grid->setName(name);
	grid->setIsInWorldSpace(false);
	grid->setVectorType(openvdb::VEC_INVARIANT);

	writer->insert(grid);

	return grid.get();
}

template <typename GridType, typename T>
void OpenVDB_import_grid(
        OpenVDBReader *reader,
        const openvdb::Name &name,
        T **data,
        const int res[3])
{
	using namespace openvdb;

	openvdb::Name temp_name = name;

	if (!reader->hasGrid(temp_name)) {
		temp_name = do_name_versionning(temp_name);

		if (!reader->hasGrid(temp_name)) {
			std::fprintf(stderr, "OpenVDB grid %s not found in file!\n", temp_name.c_str());
			memset(*data, 0, sizeof(T) * res[0] * res[1] * res[2]);
			return;
		}
	}

	typename GridType::Ptr grid = gridPtrCast<GridType>(reader->getGrid(temp_name));
	typename GridType::ConstAccessor acc = grid->getConstAccessor();

	math::Coord xyz;
	int &x = xyz[0], &y = xyz[1], &z = xyz[2];

	size_t index = 0;
	for (z = 0; z < res[2]; ++z) {
		for (y = 0; y < res[1]; ++y) {
			for (x = 0; x < res[0]; ++x, ++index) {
				(*data)[index] = acc.getValue(xyz);
			}
		}
	}
}

openvdb::GridBase *OpenVDB_export_vector_grid(OpenVDBWriter *writer,
		const openvdb::Name &name,
		const float *data_x, const float *data_y, const float *data_z,
		const int res[3],
		float fluid_mat[4][4],
		openvdb::VecType vec_type,
		const bool is_color,
		const float clipping,
		const openvdb::FloatGrid *mask);

template <typename GridType, typename T>
void OpenVDB_import_grid_extern(
        OpenVDBReader *reader,
        const openvdb::Name &name,
        T **data,
        const int res_min[3],
        const int res[3],
        short up, short front)
{
	using namespace openvdb;

	if (!reader->hasGrid(name)) {
		std::fprintf(stderr, "OpenVDB grid %s not found in file!\n", name.c_str());
		memset(*data, 0, sizeof(T) * res[0] * res[1] * res[2]);
		return;
	}

	typename GridType::Ptr grid = gridPtrCast<GridType>(reader->getGrid(name));
	typename GridType::ConstAccessor acc = grid->getConstAccessor();
	CoordBBox bbox = grid->evalActiveVoxelBoundingBox();
	Coord grid_min = bbox.getStart();
	Coord grid_max = bbox.getEnd();

	bool inv_z = up >= 3;
	bool inv_y = front < 3;
	up %= 3;
	front %= 3;
	short right = 3 - (up + front);
	bool inv_x = !(inv_z == inv_y);

	if (up < front) {
		inv_x = !inv_x;
	}

	if (abs(up - front) == 2) {
		inv_x = !inv_x;
	}

	math::Coord xyz;
	int &x = xyz[right], &y = xyz[front], &z = xyz[up];

	for (z = inv_z ? (grid_max[up] - 1) : grid_min[up];
	     inv_z ? (z >= grid_min[up]) : (z < grid_max[up]);
	     inv_z ? --z : ++z)
	{
		for (y = inv_y ? (grid_max[front] - 1) : grid_min[front];
		     inv_y ? (y >= grid_min[front]) : (y < grid_max[front]);
		     inv_y ? --y : ++y)
		{
			for (x = inv_x ? (grid_max[right] - 1) : grid_min[right];
			     inv_x ? (x >= grid_min[right]) : (x < grid_max[right]);
			     inv_x ? --x : ++x)
			{
				int x_local = x - res_min[0];
				int y_local = y - res_min[1];
				int z_local = z - res_min[2];

				int coord_local = (z_local * (res[0] * res[1])) + (y_local * res[0]) + x_local;

				(*data)[coord_local] = acc.getValue(xyz);
			}
		}
	}
}

void OpenVDB_import_grid_vector(
        OpenVDBReader *reader,
        const openvdb::Name &name,
        float **data_x, float **data_y, float **data_z,
        const int res[3]);

void OpenVDB_import_grid_vector_extern(
        OpenVDBReader *reader,
        const openvdb::Name &name,
        float **data_x, float **data_y, float **data_z,
        const int res[3]);

openvdb::CoordBBox OpenVDB_get_grid_bounds(
        OpenVDBReader *reader,
        const openvdb::Name &name);

openvdb::math::Transform::Ptr OpenVDB_get_grid_transform(
        OpenVDBReader *reader,
        const openvdb::Name &name);

}  /* namespace internal */

#endif /* __OPENVDB_DENSE_CONVERT_H__ */
