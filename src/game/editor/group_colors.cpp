// TClient: persistent editor-only group colors, stored per map in
// editor/group_colors.json inside the save storage. This is editor state,
// not part of the map file format.

#include <base/dbg.h>
#include <base/io.h>
#include <base/str.h>

#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>
#include <engine/storage.h>

#include <game/editor/editor.h>

#include <algorithm>
#include <string>
#include <vector>

static const char *const GROUP_COLORS_PATH = "editor/group_colors.json";

void CEditor::LoadGroupColors()
{
	if(!m_GroupColorsLoaded)
	{
		m_GroupColorsLoaded = true;

		IOHANDLE File = Storage()->OpenFile(GROUP_COLORS_PATH, IOFLAG_READ, IStorage::TYPE_SAVE);
		if(File != nullptr)
		{
			const int Length = io_length(File);
			if(Length > 0)
			{
				std::vector<char> Buffer(Length + 1);
				if(io_read(File, Buffer.data(), Length) == (unsigned)Length)
				{
					Buffer[Length] = '\0';
					json_value *pDoc = json_parse(Buffer.data(), Length);
					if(pDoc != nullptr)
					{
						if(pDoc->type == json_object)
						{
							const json_value *pMaps = json_object_get(pDoc, "maps");
							if(pMaps != nullptr && pMaps->type == json_object)
							{
								for(size_t i = 0; i < pMaps->u.object.length; i++)
								{
									const json_object_entry &Entry = pMaps->u.object.values[i];
									const json_value *pGroups = Entry.value;
									if(pGroups == nullptr || pGroups->type != json_array)
										continue;

									std::vector<SGroupColorEntry> vEntries;
									for(size_t g = 0; g < pGroups->u.array.length; g++)
									{
										const json_value *pGroup = pGroups->u.array.values[g];
										if(pGroup == nullptr || pGroup->type != json_array || pGroup->u.array.length < 3)
											continue;

										const json_value &Index = *pGroup->u.array.values[0];
										const json_value &Name = *pGroup->u.array.values[1];
										const json_value &Color = *pGroup->u.array.values[2];
										if(Index.type != json_integer || Color.type != json_integer || (Name.type != json_string && Name.type != json_null))
											continue;

										vEntries.push_back({(int)Index.u.integer, Name.type == json_string ? std::string(Name.u.string.ptr, Name.u.string.length) : std::string(), (int)Color.u.integer});
									}
									if(!vEntries.empty())
										m_GroupColorsByMap.emplace(Entry.name, std::move(vEntries));
								}
							}
						}
						json_value_free(pDoc);
					}
				}
			}
			io_close(File);
		}
	}

	// apply the stored colors to the current map
	CEditorMap *pMap = Map();
	auto It = m_GroupColorsByMap.find(pMap->m_aFilename);
	if(It == m_GroupColorsByMap.end())
		return;

	for(const SGroupColorEntry &Entry : It->second)
	{
		if(Entry.m_Index < 0 || Entry.m_Index >= (int)pMap->m_vpGroups.size())
			continue;
		// apply by index; if the stored name no longer matches, the group was
		// probably moved or renamed since, so skip to avoid coloring the wrong group
		if(Entry.m_aName.empty() || str_comp(Entry.m_aName.c_str(), pMap->m_vpGroups[Entry.m_Index]->m_aName) == 0)
			pMap->m_vpGroups[Entry.m_Index]->m_ColorId = Entry.m_ColorId % EditorTheme::NUM_GROUP_COLORS;
	}
}

void CEditor::SaveGroupColors()
{
	CEditorMap *pMap = Map();
	if(pMap->m_aFilename[0] == '\0')
		return; // the map was never saved, there is no name to key the colors on

	// update the in-memory cache entry of this map
	std::vector<SGroupColorEntry> vEntries;
	for(size_t g = 0; g < pMap->m_vpGroups.size(); g++)
	{
		const CLayerGroup *pGroup = pMap->m_vpGroups[g].get();
		if(pGroup->m_ColorId >= 0)
			vEntries.push_back({(int)g, std::string(pGroup->m_aName), pGroup->m_ColorId});
	}
	if(vEntries.empty())
		m_GroupColorsByMap.erase(pMap->m_aFilename);
	else
		m_GroupColorsByMap[pMap->m_aFilename] = std::move(vEntries);

	Storage()->CreateFolder("editor", IStorage::TYPE_SAVE);
	IOHANDLE File = Storage()->OpenFile(GROUP_COLORS_PATH, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(File == nullptr)
	{
		dbg_msg("editor", "Failed to write '%s'", GROUP_COLORS_PATH);
		return;
	}

	CJsonFileWriter Writer(File);
	Writer.BeginObject();
	Writer.WriteAttribute("maps");
	Writer.BeginObject();
	for(const auto &Pair : m_GroupColorsByMap)
	{
		Writer.WriteAttribute(Pair.first.c_str());
		Writer.BeginArray();
		for(const SGroupColorEntry &Entry : Pair.second)
		{
			Writer.BeginArray();
			Writer.WriteIntValue(Entry.m_Index);
			Writer.WriteStrValue(Entry.m_aName.c_str());
			Writer.WriteIntValue(Entry.m_ColorId);
			Writer.EndArray();
		}
		Writer.EndArray();
	}
	Writer.EndObject();
	Writer.EndObject();
}
