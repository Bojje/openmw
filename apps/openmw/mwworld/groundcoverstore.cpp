#include "groundcoverstore.hpp"

#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/esmloader/esmdata.hpp>
#include <components/esmloader/load.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/resource/resourcesystem.hpp>

#include "store.hpp"

#include <unordered_map>

namespace MWWorld
{
    void GroundcoverStore::init(const Store<ESM::Static>& statics, const Files::Collections& fileCollections,
        const std::vector<std::string>& groundcoverFiles, ToUTF8::Utf8Encoder* encoder, Loading::Listener* listener)
    {
        ::EsmLoader::Query query;
        query.mLoadStatics = true;
        query.mLoadCells = true;

        ESM::ReadersCache readers;
        ::EsmLoader::EsmData content
            = ::EsmLoader::loadEsmData(query, groundcoverFiles, fileCollections, readers, encoder, listener);

        static constexpr std::string_view prefix = "grass/";
        for (const ESM::Static& stat : statics)
        {
            const VFS::Path::NormalizedView model = stat.mModel.getNormalized();
            if (!model.value().starts_with(prefix))
                continue;
            mMeshCache[stat.mId] = Misc::ResourceHelpers::correctMeshPath(model);
        }

        for (const ESM::Static& stat : content.mStatics)
        {
            const VFS::Path::NormalizedView model = stat.mModel.getNormalized();
            if (!model.value().starts_with(prefix))
                continue;
            mMeshCache[stat.mId] = Misc::ResourceHelpers::correctMeshPath(model);
        }

        for (ESM::Cell& cell : content.mCells)
        {
            if (!cell.isExterior())
                continue;
            auto cellIndex = std::make_pair(cell.getGridX(), cell.getGridY());
            mCellContexts[cellIndex] = std::move(cell.mContextList);
        }
    }

    void GroundcoverStore::initCell(ESM::Cell& cell, int cellX, int cellY) const
    {
        cell.blank();

        auto searchCell = mCellContexts.find(std::make_pair(cellX, cellY));
        if (searchCell != mCellContexts.end())
            cell.mContextList = searchCell->second;
    }

    std::vector<GroundcoverRecord> GroundcoverStore::getCellRecords(int cellX, int cellY, float density) const
    {
        if (density <= 0.f)
            return {};

        ESM::Cell cell;
        initCell(cell, cellX, cellY);
        if (cell.mContextList.empty())
            return {};

        ESM::ReadersCache readers;
        std::unordered_map<ESM::RefNum, ESM::CellRef> refs;
        float currentGroundcover = 0.f;
        const auto isInstanceEnabled = [&]() {
            if (density >= 1.f)
                return true;
            currentGroundcover += density;
            if (currentGroundcover < 1.f)
                return false;
            currentGroundcover -= 1.f;
            return true;
        };

        for (std::size_t i = 0; i < cell.mContextList.size(); ++i)
        {
            const std::size_t index = static_cast<std::size_t>(cell.mContextList[i].index);
            const ESM::ReadersCache::BusyItem reader = readers.get(index);
            cell.restore(*reader, i);
            ESM::CellRef ref;
            bool deleted = false;
            while (cell.getNextRef(*reader, ref, deleted))
            {
                if (!deleted && refs.find(ref.mRefNum) == refs.end() && !isInstanceEnabled())
                    deleted = true;
                if (deleted)
                {
                    refs.erase(ref.mRefNum);
                    continue;
                }
                refs[ref.mRefNum] = std::move(ref);
            }
        }

        std::vector<GroundcoverRecord> result;
        result.reserve(refs.size());
        for (const auto& entry : refs)
        {
            const ESM::CellRef& ref = entry.second;
            const VFS::Path::NormalizedView model = getGroundcoverModel(ref.mRefID);
            if (!model.empty())
                result.push_back({ VFS::Path::Normalized(model), ref.mPos, ref.mScale });
        }
        return result;
    }
}
