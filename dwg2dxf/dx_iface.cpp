/******************************************************************************
**  dwg2dxf - Program to convert dwg/dxf to dxf(ascii & binary)              **
**                                                                           **
**  Copyright (C) 2015 José F. Soriano, rallazz@gmail.com                    **
**                                                                           **
**  This library is free software, licensed under the terms of the GNU       **
**  General Public License as published by the Free Software Foundation,     **
**  either version 2 of the License, or (at your option) any later version.  **
**  You should have received a copy of the GNU General Public License        **
**  along with this program.  If not, see <http://www.gnu.org/licenses/>.    **
******************************************************************************/

#include <iostream>
#include <algorithm>
#include <fstream>
#include "dx_iface.h"
#include "libdwgr.h"
#include "libdxfrw.h"


bool dx_iface::fileImport(const std::string& fileI, dx_data *fData, bool debug){
    unsigned int found = fileI.find_last_of(".");
    std::string fileExt = fileI.substr(found+1);
    std::transform(fileExt.begin(), fileExt.end(),fileExt.begin(), ::toupper);
    cData = fData;
    currentBlock = cData->mBlock;
    cData->fromDwg = fileExt == "DWG";

    if (fileExt == "DXF"){
        //loads dxf
        dxfRW* dxf = new dxfRW(fileI.c_str());
        if (debug) {
            dxf->setDebug(DRW::DebugLevel::Debug);
        }
        bool success = dxf->read(this, false);
        if (!success) {
            std::cout << "DXF file error: format " << dxf->getVersion() << " error " << dxf->getError() << std::endl;
        }
        delete dxf;
        return success;
    } else if (fileExt == "DWG"){
        //loads dwg
        dwgR* dwg = new dwgR(fileI.c_str());
        if (debug) {
            dwg->setDebug(DRW::DebugLevel::Debug);
        }
        bool success = dwg->read(this, false);
        cData->sourceVersion = dwg->getVersion();
        if (!success) {
            std::cout << "DWG file error: format " << dwg->getVersion() << " error " << dwg->getError() << std::endl;
        }
        delete dwg;
        return success;
    }
    std::cout << "file extension can be dxf or dwg" << std::endl;
    return false;
}

bool dx_iface::fileExport(const std::string& file, DRW::Version v, bool binary, dx_data *fData, bool debug){
    cData = fData;
    // The DXF writer allocates new handles. Materialize the saved DWG draw
    // order first, including natural ascending handles when no SORTENTSTABLE
    // exists: the DWG reader may visit entities in reverse handle order.
    // Preserve input file order for DXF and keep every block independent.
    auto reorder = [this](dx_ifaceBlock* block) {
        const auto& order = cData->drawOrder;
        if (!cData->fromDwg && !std::any_of(block->ent.begin(), block->ent.end(),
                [&order](const DRW_Entity* e) { return order.count(e->handle) != 0; })) return;
        auto key = [&order](const DRW_Entity* e) -> duint64 {
            auto found = order.find(e->handle);
            duint32 value = found == order.end() ? e->handle : found->second;
            // A zero sort handle places an entity on top; equal keys stay stable.
            return value == 0 ? (duint64(1) << 32) : value;
        };
        block->ent.sort([&key](const DRW_Entity* a, const DRW_Entity* b) { return key(a) < key(b); });
    };
    reorder(cData->mBlock);
    for (auto block : cData->blocks) reorder(block);
    // Capture original handles before the DXF writer assigns new ones.
    std::vector<std::pair<duint32, const DRW_Region*>> modelers;
    duint64 modelerBytes = 12;
    auto collectModelers = [&](dx_ifaceBlock* block) {
        for (auto entity : block->ent) {
            if (entity->eType != DRW::REGION && entity->eType != DRW::E3DSOLID
                && entity->eType != DRW::PLANESURFACE) continue;
            auto modeler = static_cast<const DRW_Region*>(entity);
            if (modeler->modelerData.empty()) continue;
            modelers.emplace_back(entity->handle, modeler);
            modelerBytes += 12 + modeler->modelerData.size();
        }
    };
    collectModelers(cData->mBlock);
    for (auto block : cData->blocks) collectModelers(block);
    if (modelerBytes > 64 * 1024 * 1024 || modelers.size() > 100000) return false;
    dxfW = new dxfRW(file.c_str());
    dxfW->setLayouts(cData->layouts);
    dxfW->setMLineStyles(cData->mlineStyles);
    dxfW->setWipeoutFrame(cData->wipeoutFrame);
    // Resolve only the named root dictionary, its variables and its scale
    // list. Unreferenced objects must never supply drawing-wide settings.
    auto& settings = cData->cadSettings;
    auto child = [&settings](const DRW_CadSetting& parent, const std::string& key) -> const DRW_CadSetting* {
        for (const auto& item : parent.items) if (item.first == key) {
            auto found = settings.find(item.second);
            if (found != settings.end()) return &found->second;
        }
        return nullptr;
    };
    for (const auto& pair : settings) if (pair.second.kind == 42 && pair.second.parentHandle == 0) {
        const auto* variables = child(pair.second, "AcDbVariableDictionary");
        const auto* scales = child(pair.second, "ACAD_SCALELIST");
        if (!variables || !scales || variables->kind != 42 || scales->kind != 42) continue;
        const auto* active = child(*variables, "CANNOSCALE");
        const auto* enabled = child(*variables, "MSLTSCALE");
        bool useScale = enabled ? enabled->value == "1" : cData->sourceVersion >= DRW::AC1024;
        if (!active || active->kind != 111 || !useScale) continue;
        for (const auto& item : scales->items) {
            auto found = settings.find(item.second);
            if (found == settings.end()) continue;
            const auto& scale = found->second;
            if (scale.kind == 112 && scale.name == active->value && scale.paperUnits > 0) {
                const double ratio = scale.drawingUnits / scale.paperUnits;
                if (std::isfinite(ratio) && ratio > 0 && ratio <= 1e9) dxfW->setModelLinetypeScale(ratio);
            }
        }
    }
    if (debug) {
        dxfW->setDebug(DRW::DebugLevel::Debug);
    }
    bool success = dxfW->write(this, v, binary);
    delete dxfW;
    if (success && !modelers.empty()) {
        std::ofstream sidecar(file + ".modeler", std::ios::binary | std::ios::trunc);
        auto uint32 = [&](duint32 value) {
            for (int shift = 0; shift < 32; shift += 8) sidecar.put(static_cast<char>((value >> shift) & 255));
        };
        sidecar.write("KOREACI1", 8);
        uint32(static_cast<duint32>(modelers.size()));
        for (const auto& entry : modelers) {
            const auto& bytes = entry.second->modelerData;
            uint32(entry.first);
            uint32(entry.second->modelerVersion);
            uint32(static_cast<duint32>(bytes.size()));
            sidecar.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
        sidecar.flush();
        success = sidecar.good();
    }
    if (success && cData->headerC.acdsData && !cData->headerC.acdsData->empty()) {
        std::ofstream sidecar(file + ".acds", std::ios::binary | std::ios::trunc);
        const auto& bytes = *cData->headerC.acdsData;
        sidecar.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        sidecar.flush();
        success = sidecar.good();
    }
    return success;
}

void dx_iface::writeEntity(DRW_Entity* e){
    switch (e->eType) {
    case DRW::RAY:
        dxfW->writeRay(static_cast<DRW_Ray*>(e));
        break;
    case DRW::XLINE:
        dxfW->writeXline(static_cast<DRW_Xline*>(e));
        break;
    case DRW::MLINE:
        dxfW->writeMLine(static_cast<DRW_MLine*>(e));
        break;
    case DRW::WIPEOUT:
        dxfW->writeWipeout(static_cast<DRW_Image*>(e));
        break;
    case DRW::ACAD_PROXY_ENTITY:
        dxfW->writeProxyEntity(static_cast<DRW_ProxyEntity*>(e));
        break;
    case DRW::E3DFACE:
        dxfW->write3dface(static_cast<DRW_3Dface*>(e));
        break;
    case DRW::VIEWPORT:
        dxfW->writeViewport(static_cast<DRW_Viewport*>(e));
        break;
    case DRW::POINT:
        dxfW->writePoint(static_cast<DRW_Point*>(e));
        break;
    case DRW::LINE:
        dxfW->writeLine(static_cast<DRW_Line*>(e));
        break;
    case DRW::REGION:
    case DRW::E3DSOLID:
    case DRW::PLANESURFACE:
        dxfW->writeModelerGeometry(static_cast<DRW_Region*>(e));
        break;
    case DRW::CIRCLE:
        dxfW->writeCircle(static_cast<DRW_Circle*>(e));
        break;
    case DRW::ARC:
        dxfW->writeArc(static_cast<DRW_Arc*>(e));
        break;
    case DRW::SOLID:
        dxfW->writeSolid(static_cast<DRW_Solid*>(e));
        break;
    case DRW::ELLIPSE:
        dxfW->writeEllipse(static_cast<DRW_Ellipse*>(e));
        break;
    case DRW::LWPOLYLINE:
        dxfW->writeLWPolyline(static_cast<DRW_LWPolyline*>(e));
        break;
    case DRW::POLYLINE:
        dxfW->writePolyline(static_cast<DRW_Polyline*>(e));
        break;
    case DRW::SPLINE:
    case DRW::HELIX:
        dxfW->writeSpline(static_cast<DRW_Spline*>(e));
        break;
//    case RS2::EntitySplinePoints:
//        writeSplinePoints(static_cast<DRW_Point*>(e));
//        break;
//    case RS2::EntityVertex:
//        break;
    case DRW::INSERT:
        dxfW->writeInsert(static_cast<DRW_Insert*>(e));
        break;
    case DRW::MTEXT:
        dxfW->writeMText(static_cast<DRW_MText*>(e));
        break;
    case DRW::TOLERANCE:
    case DRW::TEXT:
        dxfW->writeText(static_cast<DRW_Text*>(e));
        break;
    case DRW::DIMLINEAR:
    case DRW::DIMALIGNED:
    case DRW::DIMANGULAR:
    case DRW::DIMANGULAR3P:
    case DRW::DIMRADIAL:
    case DRW::DIMDIAMETRIC:
    case DRW::DIMORDINATE:
        dxfW->writeDimension(static_cast<DRW_Dimension*>(e));
        break;
    case DRW::LEADER:
        dxfW->writeLeader(static_cast<DRW_Leader*>(e));
        break;
    case DRW::HATCH:
        dxfW->writeHatch(static_cast<DRW_Hatch*>(e));
        break;
    case DRW::IMAGE:
        dxfW->writeImage(static_cast<DRW_Image*>(e), static_cast<dx_ifaceImg*>(e)->path);
        break;
    default:
        break;
    }
}
