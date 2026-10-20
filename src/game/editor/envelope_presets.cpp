// TClient: envelope presets
// Save the selected envelope as a json file and load presets into any map.
// Presets are stored in envelope_presets/ inside the save storage so they
// persist across maps and clients.

#include <base/dbg.h>
#include <base/io.h>
#include <base/math.h>
#include <base/str.h>

#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>
#include <engine/storage.h>

#include <game/editor/editor.h>
#include <game/editor/mapitems/envelope.h>
#include <game/editor/mapitems/map.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

static const char *const ENVELOPE_PRESETS_DIR = "envelope_presets";

static void EnsureEnvelopePresetsFolder(CEditor *pEditor)
{
	if(!pEditor->Storage()->FolderExists(ENVELOPE_PRESETS_DIR, IStorage::TYPE_SAVE))
		pEditor->Storage()->CreateFolder(ENVELOPE_PRESETS_DIR, IStorage::TYPE_SAVE);
}

static const char *EnvelopeTypeToString(CEnvelope::EType Type)
{
	switch(Type)
	{
	case CEnvelope::EType::POSITION: return "position";
	case CEnvelope::EType::COLOR: return "color";
	case CEnvelope::EType::SOUND: return "sound";
	}
	return "sound";
}

static bool EnvelopeTypeFromString(const char *pType, CEnvelope::EType &Result)
{
	if(str_comp_nocase(pType, "position") == 0)
	{
		Result = CEnvelope::EType::POSITION;
		return true;
	}
	if(str_comp_nocase(pType, "color") == 0)
	{
		Result = CEnvelope::EType::COLOR;
		return true;
	}
	if(str_comp_nocase(pType, "sound") == 0)
	{
		Result = CEnvelope::EType::SOUND;
		return true;
	}
	return false;
}

static int JsonToInt(const json_value *pValue, int Default)
{
	if(pValue == nullptr)
		return Default;
	if(pValue->type == json_integer)
		return (int)pValue->u.integer;
	if(pValue->type == json_double)
		return (int)std::nearbyint(pValue->u.dbl);
	return Default;
}

void CEditor::ShowSaveEnvelopePresetDialog(const char *pDefaultName)
{
	const int Selected = Map()->m_SelectedEnvelope;
	if(Selected < 0 || Selected >= (int)Map()->m_vpEnvelopes.size())
		return;

	EnsureEnvelopePresetsFolder(this);
	char aInitial[IO_MAX_PATH_LENGTH];
	str_copy(aInitial, pDefaultName);
	if(aInitial[0] == '\0')
		str_copy(aInitial, "preset");
	m_FileBrowser.ShowFileDialog(IStorage::TYPE_SAVE, CFileBrowser::EFileType::PRESET, "Save envelope preset", "Save", ENVELOPE_PRESETS_DIR, aInitial, CallbackSaveEnvelopePreset, this);
}

void CEditor::ShowLoadEnvelopePresetDialog()
{
	EnsureEnvelopePresetsFolder(this);
	// TYPE_ALL so the dialog behaves as a load dialog: the open callback only
	// fires when a file is selected in the list, never with an empty filename.
	m_FileBrowser.ShowFileDialog(IStorage::TYPE_ALL, CFileBrowser::EFileType::PRESET, "Load envelope preset", "Load", ENVELOPE_PRESETS_DIR, "", CallbackLoadEnvelopePreset, this);
}

bool CEditor::CallbackSaveEnvelopePreset(const char *pFilename, int StorageType, void *pUser)
{
	dbg_assert(StorageType == IStorage::TYPE_SAVE, "Saving only allowed for IStorage::TYPE_SAVE");
	CEditor *pEditor = static_cast<CEditor *>(pUser);

	const int Selected = pEditor->Map()->m_SelectedEnvelope;
	if(Selected < 0 || Selected >= (int)pEditor->Map()->m_vpEnvelopes.size())
	{
		pEditor->ShowFileDialogError("No envelope is selected.");
		return false;
	}
	const std::shared_ptr<CEnvelope> pEnvelope = pEditor->Map()->m_vpEnvelopes[Selected];

	IOHANDLE File = pEditor->Storage()->OpenFile(pFilename, IOFLAG_WRITE, StorageType);
	if(File == nullptr)
	{
		pEditor->ShowFileDialogError("Failed to write file '%s'.", pFilename);
		return false;
	}

	{
		CJsonFileWriter Writer(File);
		Writer.BeginObject();
		Writer.WriteAttribute("format");
		Writer.WriteIntValue(1);
		Writer.WriteAttribute("envelopes");
		Writer.BeginArray();
		Writer.BeginObject();
		Writer.WriteAttribute("name");
		Writer.WriteStrValue(pEnvelope->m_aName);
		Writer.WriteAttribute("type");
		Writer.WriteStrValue(EnvelopeTypeToString(pEnvelope->Type()));
		Writer.WriteAttribute("synchronized");
		Writer.WriteBoolValue(pEnvelope->m_Synchronized);
		Writer.WriteAttribute("points");
		Writer.BeginArray();
		for(const CEnvPoint_runtime &Point : pEnvelope->m_vPoints)
		{
			Writer.BeginObject();
			Writer.WriteAttribute("time_ms");
			Writer.WriteIntValue(Point.m_Time.GetInternal());
			Writer.WriteAttribute("curve");
			Writer.WriteIntValue(Point.m_Curvetype);
			Writer.WriteAttribute("values");
			Writer.BeginArray();
			for(int c = 0; c < pEnvelope->GetChannels(); c++)
			{
				Writer.WriteIntValue(Point.m_aValues[c]);
			}
			Writer.EndArray();
			if(Point.m_Curvetype == CURVETYPE_BEZIER)
			{
				Writer.WriteAttribute("bezier");
				Writer.BeginObject();
				Writer.WriteAttribute("in_x");
				Writer.BeginArray();
				for(int c = 0; c < pEnvelope->GetChannels(); c++)
				{
					Writer.WriteIntValue(Point.m_Bezier.m_aInTangentDeltaX[c].GetInternal());
				}
				Writer.EndArray();
				Writer.WriteAttribute("in_y");
				Writer.BeginArray();
				for(int c = 0; c < pEnvelope->GetChannels(); c++)
				{
					Writer.WriteIntValue(Point.m_Bezier.m_aInTangentDeltaY[c]);
				}
				Writer.EndArray();
				Writer.WriteAttribute("out_x");
				Writer.BeginArray();
				for(int c = 0; c < pEnvelope->GetChannels(); c++)
				{
					Writer.WriteIntValue(Point.m_Bezier.m_aOutTangentDeltaX[c].GetInternal());
				}
				Writer.EndArray();
				Writer.WriteAttribute("out_y");
				Writer.BeginArray();
				for(int c = 0; c < pEnvelope->GetChannels(); c++)
				{
					Writer.WriteIntValue(Point.m_Bezier.m_aOutTangentDeltaY[c]);
				}
				Writer.EndArray();
				Writer.EndObject();
			}
			Writer.EndObject();
		}
		Writer.EndArray();
		Writer.EndObject();
		Writer.EndArray();
		Writer.EndObject();
	}

	pEditor->OnDialogClose();
	return true;
}

bool CEditor::CallbackLoadEnvelopePreset(const char *pFilename, int StorageType, void *pUser)
{
	CEditor *pEditor = static_cast<CEditor *>(pUser);

	IOHANDLE File = pEditor->Storage()->OpenFile(pFilename, IOFLAG_READ, StorageType);
	if(File == nullptr)
	{
		pEditor->ShowFileDialogError("Failed to open file '%s'.", pFilename);
		return false;
	}
	const int Length = io_length(File);
	std::vector<char> Buffer(std::max(Length + 1, 1));
	if(Length <= 0 || io_read(File, Buffer.data(), Length) != (unsigned)Length)
	{
		io_close(File);
		pEditor->ShowFileDialogError("Failed to read file '%s'.", pFilename);
		return false;
	}
	io_close(File);
	Buffer[Length] = '\0';

	json_value *pDoc = json_parse(Buffer.data(), Length);
	if(pDoc == nullptr || pDoc->type != json_object)
	{
		json_value_free(pDoc);
		pEditor->ShowFileDialogError("Failed to parse envelope preset '%s'.", pFilename);
		return false;
	}

	const json_value *pEnvelopes = json_object_get(pDoc, "envelopes");
	if(pEnvelopes == nullptr || pEnvelopes->type != json_array || pEnvelopes->u.array.length == 0)
	{
		json_value_free(pDoc);
		pEditor->ShowFileDialogError("Envelope preset '%s' contains no envelopes.", pFilename);
		return false;
	}

	int NumLoaded = 0;
	for(size_t i = 0; i < pEnvelopes->u.array.length; i++)
	{
		const json_value *pEnvObject = pEnvelopes->u.array.values[i];
		if(pEnvObject == nullptr || pEnvObject->type != json_object)
			continue;

		const json_value *pType = json_object_get(pEnvObject, "type");
		CEnvelope::EType Type = CEnvelope::EType::SOUND;
		if(pType == nullptr || pType->type != json_string || !EnvelopeTypeFromString(json_string_get(pType), Type))
		{
			pEditor->ShowFileDialogError("Envelope preset '%s' contains an envelope with an unknown type.", pFilename);
			continue;
		}

		auto pEnvelope = std::make_shared<CEnvelope>(Type);

		const json_value *pName = json_object_get(pEnvObject, "name");
		if(pName != nullptr && pName->type == json_string)
			str_copy(pEnvelope->m_aName, json_string_get(pName));

		const json_value *pSynchronized = json_object_get(pEnvObject, "synchronized");
		pEnvelope->m_Synchronized = pSynchronized == nullptr || (pSynchronized->type != json_boolean || pSynchronized->u.boolean);

		const json_value *pPoints = json_object_get(pEnvObject, "points");
		if(pPoints == nullptr || pPoints->type != json_array)
		{
			pEditor->Map()->InsertEnvelope((int)pEditor->Map()->m_vpEnvelopes.size(), pEnvelope);
			NumLoaded++;
			continue;
		}

		for(size_t p = 0; p < pPoints->u.array.length; p++)
		{
			const json_value *pPointObject = pPoints->u.array.values[p];
			if(pPointObject == nullptr || pPointObject->type != json_object)
				continue;

			CEnvPoint_runtime Point{};
			Point.m_Time = CFixedTime(JsonToInt(json_object_get(pPointObject, "time_ms"), 0));
			Point.m_Curvetype = std::clamp(JsonToInt(json_object_get(pPointObject, "curve"), CURVETYPE_LINEAR), 0, NUM_CURVETYPES - 1);

			const json_value *pValues = json_object_get(pPointObject, "values");
			if(pValues != nullptr && pValues->type == json_array)
			{
				const size_t NumValues = std::min((size_t)pValues->u.array.length, (size_t)CEnvPoint::MAX_CHANNELS);
				for(size_t c = 0; c < NumValues; c++)
				{
					Point.m_aValues[c] = JsonToInt(pValues->u.array.values[c], 0);
				}
			}

			if(Point.m_Curvetype == CURVETYPE_BEZIER)
			{
				const json_value *pBezier = json_object_get(pPointObject, "bezier");
				if(pBezier != nullptr && pBezier->type == json_object)
				{
					const json_value *pInX = json_object_get(pBezier, "in_x");
					const json_value *pInY = json_object_get(pBezier, "in_y");
					const json_value *pOutX = json_object_get(pBezier, "out_x");
					const json_value *pOutY = json_object_get(pBezier, "out_y");
					for(int c = 0; c < pEnvelope->GetChannels(); c++)
					{
						if(pInX != nullptr && pInX->type == json_array && c < (int)pInX->u.array.length)
							Point.m_Bezier.m_aInTangentDeltaX[c] = CFixedTime(JsonToInt(pInX->u.array.values[c], 0));
						if(pInY != nullptr && pInY->type == json_array && c < (int)pInY->u.array.length)
							Point.m_Bezier.m_aInTangentDeltaY[c] = JsonToInt(pInY->u.array.values[c], 0);
						if(pOutX != nullptr && pOutX->type == json_array && c < (int)pOutX->u.array.length)
							Point.m_Bezier.m_aOutTangentDeltaX[c] = CFixedTime(JsonToInt(pOutX->u.array.values[c], 0));
						if(pOutY != nullptr && pOutY->type == json_array && c < (int)pOutY->u.array.length)
							Point.m_Bezier.m_aOutTangentDeltaY[c] = JsonToInt(pOutY->u.array.values[c], 0);
					}
				}
			}

			pEnvelope->m_vPoints.push_back(Point);
		}

		std::sort(pEnvelope->m_vPoints.begin(), pEnvelope->m_vPoints.end());

		pEditor->Map()->InsertEnvelope((int)pEditor->Map()->m_vpEnvelopes.size(), pEnvelope);
		NumLoaded++;
	}

	json_value_free(pDoc);

	if(NumLoaded == 0)
	{
		pEditor->ShowFileDialogError("No envelopes could be loaded from '%s'.", pFilename);
		return false;
	}

	pEditor->Map()->OnModify();
	pEditor->OnDialogClose();
	return true;
}
