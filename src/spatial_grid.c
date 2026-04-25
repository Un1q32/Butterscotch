#include "spatial_grid.h"

#include "collision.h"
#include "instance.h"
#include "runner.h"
#include "utils.h"

// Forward declarations
typedef struct Runner Runner;

SpatialGrid* SpatialGrid_create(uint32_t roomWidth, uint32_t roomHeight) {
    SpatialGrid* grid = safeCalloc(1, sizeof(SpatialGrid));

    // +1 to avoid truncation
    uint32_t gridWidth = (roomWidth / SPATIAL_GRID_CELL_SIZE) + 1;
    uint32_t gridHeight = (roomHeight / SPATIAL_GRID_CELL_SIZE) + 1;

    fprintf(stderr, "SpatialGrid: Grid size: %dx%d\n", gridWidth, gridHeight);
    grid->gridWidth = gridWidth;
    grid->gridHeight = gridHeight;

    grid->grid = safeCalloc(gridWidth, sizeof(int32_t*));
    repeat(gridWidth, i) {
        grid->grid[i] = safeCalloc(gridHeight, sizeof(int32_t*));
    }

    return grid;
}

void SpatialGrid_free(SpatialGrid* grid) {
    repeat(grid->gridWidth, i) {
        repeat(grid->gridHeight, j) {
            arrfree(grid->grid[i][j]);
        }
        free(grid->grid[i]);
    }
    free(grid->grid);
    free(grid);
}

static void removeInstanceFromGridCells(SpatialGrid* grid, Instance* instance) {
    repeat(arrlen(instance->collisionCells), i) {
        uint32_t gridCoordinates = instance->collisionCells[i];
        int32_t gridX = SpatialGrid_unpackGridX(gridCoordinates);
        int32_t gridY = SpatialGrid_unpackGridY(gridCoordinates);
        int32_t* cell = grid->grid[gridX][gridY];
        repeat(arrlen(cell), j) {
            if (cell[j] == instance->instanceId) {
                arrdel(grid->grid[gridX][gridY], j);
                break;
            }
        }
    }
}

void SpatialGrid_syncGrid(Runner* runner, SpatialGrid* grid) {
    bool requiresResync = arrlen(grid->dirtyInstances);
    if (!requiresResync) return;

    fprintf(stderr, "SpatialGrid: Syncing grid with %d dirty instances\n", arrlen(grid->dirtyInstances));

    repeat(arrlen(grid->dirtyInstances), i) {
        int32_t instanceId = grid->dirtyInstances[i];
        Instance* instance = hmget(runner->instancesToId, instanceId);

        // We do not care about removed/inactive/destroyed instances, because they would've been already been removed from the grid on the "SpatialGrid_markInstanceAsDirty" call
        // We also do not care if the spatial grid is not dirty
        if (instance == nullptr || !instance->active || instance->destroyed || !instance->spatialGridDirty)
            continue;

        instance->spatialGridDirty = false;

        // Remove from old cells
        removeInstanceFromGridCells(grid, instance);

        InstanceBBox bbox = Collision_computeBBox(runner->dataWin, instance);

        arrsetlen(instance->collisionCells, 0);

        if (!bbox.valid)
            continue;

        SpatialGridRange range = SpatialGrid_computeCellRange(grid, bbox.left, bbox.top, bbox.right, bbox.bottom);

        for (int32_t gx = range.minGridX; range.maxGridX >= gx; gx++) {
            for (int32_t gy = range.minGridY; range.maxGridY >= gy; gy++) {
                arrput(grid->grid[gx][gy], instance->instanceId);
                arrput(instance->collisionCells, SpatialGrid_packGridCoordinates(gx, gy));
            }
        }

        // And that's it for now!
    }

    arrsetlen(grid->dirtyInstances, 0);
}

void SpatialGrid_markInstanceAsDirty(SpatialGrid* grid, Instance* dirtyInstance) {
    if (!dirtyInstance->active || dirtyInstance->destroyed) {
        // Destroyed instances are updated instantly because, if we didn't, we would need to track the ID + all grids that the instance is in
        removeInstanceFromGridCells(grid, dirtyInstance);
        return;
    }

    if (dirtyInstance->spatialGridDirty)
        return;

    dirtyInstance->spatialGridDirty = true;

    arrput(grid->dirtyInstances, dirtyInstance->instanceId);
}