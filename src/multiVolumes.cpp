//Copyright (c) 2021 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#include "multiVolumes.h"

#include <algorithm>

#include "Application.h"
#include "Slice.h"
#include "slicer.h"
#include "utils/PolylineStitcher.h"
#include "settings/EnumSettings.h"

namespace cura 
{
 
void carveMultipleVolumes(std::vector<Slicer*> &volumes)
{
    //Go trough all the volumes, and remove the previous volume outlines from our own outline, so we never have overlapped areas.
    const bool alternate_carve_order = Application::getInstance().current_slice->scene.current_mesh_group->settings.get<bool>("alternate_carve_order");
    std::vector<Slicer*> ranked_volumes = volumes;
    std::sort(ranked_volumes.begin(), ranked_volumes.end(),
              [](Slicer* volume_1, Slicer* volume_2)
                {
                    return volume_1->mesh->settings.get<int>("infill_mesh_order") < volume_2->mesh->settings.get<int>("infill_mesh_order");
                } );
    for (unsigned int volume_1_idx = 1; volume_1_idx < volumes.size(); volume_1_idx++)
    {
        Slicer& volume_1 = *ranked_volumes[volume_1_idx];
        if (volume_1.mesh->settings.get<bool>("infill_mesh") 
            || volume_1.mesh->settings.get<bool>("anti_overhang_mesh")
            || volume_1.mesh->settings.get<bool>("support_mesh")
            || volume_1.mesh->settings.get<ESurfaceMode>("magic_mesh_surface_mode") == ESurfaceMode::SURFACE
            )
        {
            continue;
        }
        for (unsigned int volume_2_idx = 0; volume_2_idx < volume_1_idx; volume_2_idx++)
        {
            Slicer& volume_2 = *ranked_volumes[volume_2_idx];
            if (volume_2.mesh->settings.get<bool>("infill_mesh")
                || volume_2.mesh->settings.get<bool>("anti_overhang_mesh")
                || volume_2.mesh->settings.get<bool>("support_mesh")
                || volume_2.mesh->settings.get<ESurfaceMode>("magic_mesh_surface_mode") == ESurfaceMode::SURFACE
                )
            {
                continue;
            }
            if (!volume_1.mesh->getAABB().hit(volume_2.mesh->getAABB()))
            {
                continue;
            }
            for (unsigned int layerNr = 0; layerNr < volume_1.layers.size(); layerNr++)
            {
                SlicerLayer& layer1 = volume_1.layers[layerNr];
                SlicerLayer& layer2 = volume_2.layers[layerNr];
                if (alternate_carve_order && layerNr % 2 == 0 && volume_1.mesh->settings.get<int>("infill_mesh_order") == volume_2.mesh->settings.get<int>("infill_mesh_order"))
                {
                    layer2.polygons = layer2.polygons.difference(layer1.polygons);
                }
                else
                {
                    layer1.polygons = layer1.polygons.difference(layer2.polygons);
                }
            }
        }
    }
}
 
//Expand each layer a bit and then keep the extra overlapping parts that overlap with other volumes.
//This generates some overlap in dual extrusion, for better bonding in touching parts.
void generateMultipleVolumesOverlap(std::vector<Slicer*> &volumes)
{
    if (volumes.size() < 2)
    {
        return;
    }

    int offset_to_merge_other_merged_volumes = 20;
    for (Slicer* volume : volumes)
    {
        ClipperLib::PolyFillType fill_type = volume->mesh->settings.get<bool>("meshfix_union_all") ? ClipperLib::pftNonZero : ClipperLib::pftEvenOdd;

        coord_t overlap = volume->mesh->settings.get<coord_t>("multiple_mesh_overlap");
        if (volume->mesh->settings.get<bool>("infill_mesh")
            || volume->mesh->settings.get<bool>("anti_overhang_mesh")
            || volume->mesh->settings.get<bool>("support_mesh")
            || overlap == 0)
        {
            continue;
        }
        AABB3D aabb(volume->mesh->getAABB());
        aabb.expandXY(overlap); // expand to account for the case where two models and their bounding boxes are adjacent along the X or Y-direction
        for (unsigned int layer_nr = 0; layer_nr < volume->layers.size(); layer_nr++)
        {
            Polygons all_other_volumes;
            for (Slicer* other_volume : volumes)
            {
                if (other_volume->mesh->settings.get<bool>("infill_mesh")
                    || other_volume->mesh->settings.get<bool>("anti_overhang_mesh")
                    || other_volume->mesh->settings.get<bool>("support_mesh")
                    || !other_volume->mesh->getAABB().hit(aabb)
                    || other_volume == volume
                )
                {
                    continue;
                }
                SlicerLayer& other_volume_layer = other_volume->layers[layer_nr];
                all_other_volumes = all_other_volumes.unionPolygons(other_volume_layer.polygons.offset(offset_to_merge_other_merged_volumes), fill_type);
            }

            SlicerLayer& volume_layer = volume->layers[layer_nr];
            volume_layer.polygons = volume_layer.polygons.unionPolygons(all_other_volumes.intersection(volume_layer.polygons.offset(overlap / 2)), fill_type);
        }
    }
}

void MultiVolumes::carveCuttingMeshes(std::vector<Slicer*>& volumes, const std::vector<Mesh>& meshes)
{
    for (unsigned int carving_mesh_idx = 0; carving_mesh_idx < volumes.size(); carving_mesh_idx++)
    {
        const Mesh& cutting_mesh = meshes[carving_mesh_idx];
        if (!cutting_mesh.settings.get<bool>("cutting_mesh"))
        {
            continue;
        }
        Slicer& cutting_mesh_volume = *volumes[carving_mesh_idx];
        for (unsigned int layer_nr = 0; layer_nr < cutting_mesh_volume.layers.size(); layer_nr++)
        {
            Polygons& cutting_mesh_polygons = cutting_mesh_volume.layers[layer_nr].polygons;
            Polygons& cutting_mesh_polylines = cutting_mesh_volume.layers[layer_nr].openPolylines;
            Polygons cutting_mesh_area_recomputed;
            Polygons* cutting_mesh_area;
            coord_t surface_line_width = cutting_mesh.settings.get<coord_t>("wall_line_width_0");
            { // compute cutting_mesh_area
                if (cutting_mesh.settings.get<ESurfaceMode>("magic_mesh_surface_mode") == ESurfaceMode::BOTH)
                {
                    cutting_mesh_area_recomputed = cutting_mesh_polygons.unionPolygons(cutting_mesh_polylines.offsetPolyLine(surface_line_width / 2));
                    cutting_mesh_area = &cutting_mesh_area_recomputed;
                }
                else if (cutting_mesh.settings.get<ESurfaceMode>("magic_mesh_surface_mode") == ESurfaceMode::SURFACE)
                {
                    // break up polygons into polylines
                    // they have to be polylines, because they might break up further when doing the cutting
                    for (PolygonRef poly : cutting_mesh_polygons)
                    {
                        poly.add(poly[0]);
                    }
                    cutting_mesh_polylines.add(cutting_mesh_polygons);
                    cutting_mesh_polygons.clear();
                    cutting_mesh_area_recomputed = cutting_mesh_polylines.offsetPolyLine(surface_line_width / 2);
                    cutting_mesh_area = &cutting_mesh_area_recomputed;
                }
                else
                {
                    cutting_mesh_area = &cutting_mesh_polygons;
                }
            }
            
            Polygons new_outlines;
            Polygons new_polylines;
            for (unsigned int carved_mesh_idx = 0; carved_mesh_idx < volumes.size(); carved_mesh_idx++)
            {
                const Mesh& carved_mesh = meshes[carved_mesh_idx];
                //Do not apply cutting_mesh for meshes which have settings (cutting_mesh, anti_overhang_mesh, support_mesh).
                if (carved_mesh.settings.get<bool>("cutting_mesh") || carved_mesh.settings.get<bool>("anti_overhang_mesh")
                    || carved_mesh.settings.get<bool>("support_mesh"))
                {
                    continue;
                }
                Slicer& carved_volume = *volumes[carved_mesh_idx];
                Polygons& carved_mesh_layer = carved_volume.layers[layer_nr].polygons;

                Polygons intersection = cutting_mesh_polygons.intersection(carved_mesh_layer);
                new_outlines.add(intersection);
                if (cutting_mesh.settings.get<ESurfaceMode>("magic_mesh_surface_mode") != ESurfaceMode::NORMAL) // niet te geleuven
                {
                    new_polylines.add(carved_mesh_layer.intersectionPolyLines(cutting_mesh_polylines));
                }

                carved_mesh_layer = carved_mesh_layer.difference(*cutting_mesh_area);
            }
            cutting_mesh_polygons = new_outlines.unionPolygons();
            if (cutting_mesh.settings.get<ESurfaceMode>("magic_mesh_surface_mode") != ESurfaceMode::NORMAL)
            {
                cutting_mesh_polylines.clear();
                cutting_mesh_polygons.clear();
                PolylineStitcher<Polygons, Polygon, Point>::stitch(new_polylines, cutting_mesh_polylines, cutting_mesh_polygons, surface_line_width);
            }
        }
    }
}


}//namespace cura
